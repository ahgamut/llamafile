// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=c++ ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "llamafile.h"
#include "log.h"

#include <cosmo.h>
#include <fenv.h>
#include <libc/calls/struct/aarch64.internal.h>
#include <libc/calls/struct/ucontext.internal.h>
#include <signal.h>
#include <stdatomic.h>
#include <termios.h>
#include <ucontext.h>
#include <unistd.h>

#include "llama.cpp/ggml.h"

static int get_config(void) {
    int config = FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW;
    // TODO(jart): enable if quantization isn't being used
    //             or possibly suppress exceptions in quants
    // config |= FE_UNDERFLOW;
    return config;
}

bool FLAG_trap;
static thread_local int g_enabled;
thread_local int llamafile_debug_op_index;
const struct ggml_cgraph *llamafile_debug_graph;

static struct TerminalBuddy {
    TerminalBuddy() {
        active = !tcgetattr(0, &state);
    }
    void restore() {
        if (active)
            tcsetattr(0, TCSANOW, &state);
    }
    bool active;
    struct termios state;
} g_terminal_buddy;

static const char *describe_vertex(struct ggml_tensor *t) {
    if (t->op == GGML_OP_UNARY)
        return ggml_unary_op_name((enum ggml_unary_op)t->op_params[0]);
    if (t->op)
        return ggml_op_name(t->op);
    return t->name;
}

static void print_graph(FILE *f, const struct ggml_cgraph *g) {
    for (int i = 0; i < g->n_nodes; ++i) {
        fprintf(f, "%5d %p %s:%s(", i, g->nodes[i], ggml_type_name(g->nodes[i]->type),
                describe_vertex(g->nodes[i]));
        for (int j = 0; j < GGML_MAX_SRC && g->nodes[i]->src[j]; ++j) {
            if (j)
                fprintf(f, ", ");
            fprintf(f, "%s:%s[%p]", ggml_type_name(g->nodes[i]->src[j]->type),
                    describe_vertex(g->nodes[i]->src[j]), g->nodes[i]->src[j]);
        }
        fprintf(f, ")\n");
    }
}

int feenableexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
#ifdef __x86_64__
    unsigned mxcsr;
    asm("stmxcsr\t%0" : "=m"(mxcsr));
    mxcsr &= ~(excepts << 7);
    asm("ldmxcsr\t%0" : /* no inputs */ : "m"(mxcsr));
#endif
    return 0;
}

int fedisableexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
#ifdef __x86_64__
    unsigned mxcsr;
    asm("stmxcsr\t%0" : "=m"(mxcsr));
    mxcsr |= excepts << 7;
    asm("ldmxcsr\t%0" : /* no inputs */ : "m"(mxcsr));
#endif
    return 0;
}

static inline void spinlock(atomic_uint *lock) {
    int x;
    for (;;) {
        x = atomic_exchange_explicit(lock, 1, memory_order_acquire);
        if (!x)
            break;
    }
}

static inline void spunlock(atomic_uint *lock) {
    atomic_store_explicit(lock, 0, memory_order_release);
}

static void on_sigfpe(int sig, siginfo_t *si, void *arg) {

    static atomic_uint lock;
    spinlock(&lock);

    const char *issue;
    ucontext_t *ctx = (ucontext_t *)arg;
    if (si->si_code == FPE_INTDIV)
        issue = "integer divide by zero";
    else if (si->si_code == FPE_INTOVF)
        issue = "integer overflow";
    else if (si->si_code == FPE_FLTDIV)
        issue = "floating point divide by zero";
    else if (si->si_code == FPE_FLTOVF)
        issue = "floating point overflow";
    else if (si->si_code == FPE_FLTUND)
        issue = "floating point underflow";
    else if (si->si_code == FPE_FLTRES)
        issue = "floating point inexact";
    else if (si->si_code == FPE_FLTINV)
        issue = "invalid floating point operation";
    else if (si->si_code == FPE_FLTSUB)
        issue = "subscript out of range";
    else
        issue = "sigfpe";

    // show detailed information
    static bool once;
    char location[21];
    const char *path = "/tmp/cgraph.txt";
    int idx = llamafile_debug_op_index;
    const struct ggml_cgraph *g;
    if ((g = llamafile_debug_graph)) {
        FormatInt32(location, idx);
        tinyprint(2, "error: ", issue, " at ", ggml_op_name(g->nodes[idx]->op), " operation in ",
                  path, " at index #", location, "\n", NULL);
    } else {
        FormatHex64(location, ctx->uc_mcontext.PC, 2);
        tinyprint(2, "error: ", issue, " at pc ", location, "\n", NULL);
    }
    if (!once) {
        struct StackFrame sf = {.next = (struct StackFrame *)ctx->uc_mcontext.BP,
                                .addr = ctx->uc_mcontext.PC};
        ShowBacktrace(2, &sf);
        const struct ggml_cgraph *g;
        if ((g = llamafile_debug_graph)) {
            FILE *f = fopen(path, "w");
            print_graph(f, g);
            fclose(f);
        }
        once = true;
    }

    // recover from trap so that execution may resume
    // without this the same signal will just keep getting raised
    int traps = get_config();
#ifdef __x86_64__
    if (ctx->uc_mcontext.fpregs) {
        ctx->uc_mcontext.fpregs->mxcsr |= traps << 7; // disable traps
        ctx->uc_mcontext.fpregs->mxcsr &= ~traps; // clear cages
        spunlock(&lock);
        return;
    }
#elif defined(__aarch64__)
    struct _aarch64_ctx *ac;
    for (ac = (struct _aarch64_ctx *)ctx->uc_mcontext.__reserved; ac->magic;
         ac = (struct _aarch64_ctx *)((char *)ac + ac->size)) {
        if (ac->magic == FPSIMD_MAGIC) {
            struct fpsimd_context *sm = (struct fpsimd_context *)ac;
            sm->fpcr &= ~(traps << 8); // disable traps
            sm->fpsr &= ~traps; // clear cages
            spunlock(&lock);
            return;
        }
    }
#endif

    // time to die
    g_terminal_buddy.restore();
    _exit(128 + SIGFPE);
}

static void setup_sigfpe(void) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = on_sigfpe;
    if (sigaction(SIGFPE, &sa, 0)) {
        perror("sigaction(SIGFPE)");
        exit(1);
    }
}

int llamafile_trapping_enabled(int delta) {
    static atomic_uint once;
    bool was_enabled = g_enabled > 0;
    bool is_enabled = (g_enabled += delta) > 0;
    int config = get_config();
    feclearexcept(FE_ALL_EXCEPT);
    if (is_enabled && !was_enabled) {
        cosmo_once(&once, setup_sigfpe);
        feenableexcept(config);
    }
    if (!is_enabled && was_enabled) {
        fedisableexcept(config);
    }
    return g_enabled;
}

void llamafile_trapping_restore(void) {
    if (g_enabled > 0) {
        feclearexcept(FE_ALL_EXCEPT);
        feenableexcept(get_config());
    }
}
