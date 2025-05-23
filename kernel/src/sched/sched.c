#include "sched.h"

#include <mem/kmem.h>
#include <stdint.h>

#include "arch/arch.h"
#include "arch/x86_64/cpu/lapic.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/instructions/instructions.h"
#include "fs/vfs/fd.h"
#include "fs/vfs/vfs.h"
#include "mem/vmm.h"
#include "sched.h"
#include "sched/reaper.h"
#include "smp/smp.h"
#include "utils/basic.h"
#include <elf/elf.h>
// ARCH STUFF
#include "arch/x86_64/cpu/structures.h"
#include <arch/x86_64/fpu/xsave.h>

#if defined(__x86_64__)
struct per_cpu_data bsp = {.run_queue = NULL};
#endif
struct per_cpu_data *arch_get_per_cpu_data() {
#if defined(__x86_64__)
  struct per_cpu_data *hi = (struct per_cpu_data *)rdmsr(0xC0000101);
  return hi;
#endif
}
void arch_create_bsp_per_cpu_data() { wrmsr(0xC0000101, (uint64_t)&bsp); }
void arch_create_per_cpu_data() {
#if defined(__x86_64__)
  struct per_cpu_data *hey =
      (struct per_cpu_data *)kmalloc(sizeof(struct per_cpu_data));
  hey->run_queue = NULL;
  hey->arch_data.lapic_id = get_lapic_id();
  hey->arch_data.kernel_stack_ptr = 0;
  hey->arch_data.syscall_stack_ptr_tmp = 0;
  wrmsr(0xC0000101, (uint64_t)hey);
#endif
}
struct process_t *create_process(pagemap *map) {
  struct process_t *him = (struct process_t *)kmalloc(sizeof(struct process_t));
  him->cur_map = map;
  him->lock = SPINLOCK_INITIALIZER;
  him->cnt = 0;
  him->fds = hashmap_new(sizeof(struct FileDescriptorHandle), 0, 0, 0, fd_hash,
                         fd_compare, NULL, NULL);
  him->fdalloc[0] = 1;
  him->fdalloc[1] = 1;
  him->fdalloc[2] = 1;
  if (vfs_list) {

    him->root = vfs_list->cur_vnode;
    him->cwd = him->root;
    him->cwdpath = "/";
  }
  hashmap_set(him->fds, &(struct FileDescriptorHandle){.fd = 0});
  hashmap_set(him->fds, &(struct FileDescriptorHandle){.fd = 1});
  hashmap_set(him->fds, &(struct FileDescriptorHandle){.fd = 2});
  return him;
}

struct thread_t *create_thread() {
  struct thread_t *him = (struct thread_t *)kmalloc(sizeof(struct thread_t));
  him->count = 0;
  // him->next = NULL;
  return him;
}
void push_into_list(struct thread_t **list, struct thread_t *whatuwannapush) {
  if (*list == NULL) {
    (*list) = whatuwannapush;
    (*list)->next = *list;
    (*list)->back = *list;
    return;
  }
  struct thread_t *temp = (*list);
  struct thread_t *old = (*list);
  while (temp->next != old) {
    temp = temp->next;
  }
  temp->next = whatuwannapush;
  whatuwannapush->back = temp;
  whatuwannapush->next = old;
  old->back = whatuwannapush;
}
struct thread_t *pop_from_list(struct thread_t **list) {
  struct thread_t *old = *list;
  *list = (*list)->next;
  (*list)->back = old->back;
  (*list)->back->next = *list;
  return old;
}

void exit_thread() {
  __asm__ volatile("cli");
  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  cpu->cur_thread->state = ZOMBIE;
  assert(cpu->cur_thread->proc != NULL);

  // assert(cpu->to_be_reapered != NULL);
  assert(cpu->cur_thread->proc != NULL);
  refcount_dec(&cpu->cur_thread->proc->cnt);
  refcount_dec(&cpu->cur_thread->count);
  refcount_dec(&cpu->cur_thread->count);

  cpu->cur_thread->next = cpu->to_be_reapered;
  cpu->to_be_reapered = cpu->cur_thread;

  // hcf();
  // no need to assert
  // reaper thread is always running
  sched_yield();
}

void ThreadBlock(struct thread_t *whichqueue) {
  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  cpu->cur_thread->state = BLOCKED;
  push_into_list(&whichqueue, cpu->cur_thread);
}
void ThreadReady(struct thread_t *thread) {
  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  thread->state = READYING;

  push_into_list(&cpu->run_queue, thread);
}
void create_kthread(uint64_t entry, struct process_t *proc, uint64_t tid) {
  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  struct thread_t *newthread = create_thread();
  newthread->proc = proc;
  newthread->tid = tid;
  refcount_inc(&proc->cnt);
  uint64_t kstack = (uint64_t)(kmalloc(KSTACKSIZE) + KSTACKSIZE);
  struct StackFrame hh = arch_create_frame(false, entry, kstack - 8);
  newthread->kernel_stack_base = kstack;
  newthread->kernel_stack_ptr = kstack;
  newthread->arch_data.frame = hh;
  refcount_inc(&newthread->count);
  refcount_inc(&newthread->count);
  ThreadReady(newthread);
}
void build_fpu_state(void *area) {
  struct fpu_state *state = area;
  state->fcw |= 0x33F;
  state->mxcsr |= 0x1F80;
}
struct thread_t *create_uthread(uint64_t entry, struct process_t *proc,
                                uint64_t tid) {
  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  struct thread_t *newthread = create_thread();
  newthread->proc = proc;
  newthread->tid = tid;
  newthread->fpu_state = (void *)align_up(
      (uint64_t)kmalloc(align_up(get_fpu_storage_size(), 0x1000) + 64), 64);
  kprintf("fpu state %p\r\n", (void *)newthread->fpu_state);
  build_fpu_state(newthread->fpu_state);
  refcount_inc(&proc->cnt);
  uint64_t kstack = (uint64_t)(kmalloc(KSTACKSIZE) + KSTACKSIZE);
  uint64_t ustack =
      (uint64_t)uvmm_region_alloc(proc->cur_map, KSTACKSIZE, 0) + KSTACKSIZE;
  struct StackFrame hh = arch_create_frame(true, entry, ustack - 8);
  newthread->kernel_stack_base = kstack;
  newthread->kernel_stack_ptr = kstack;
  build_fpu_state(newthread->fpu_state);
  newthread->arch_data.frame = hh;
  refcount_inc(&newthread->count);
  refcount_inc(&newthread->count);
  return newthread;
}

extern void shitfuck();
void create_kentry() {
  hashmap_set_allocator(kmalloc, kfree);
#if defined(__x86_64__)
  struct process_t *kernelprocess = create_process(&ker_map);

  create_kthread((uint64_t)kentry, kernelprocess, 1);
  create_kthread((uint64_t)reaper, kernelprocess, 0);

  // create_kthread((uint64_t)klocktest, kernelprocess, 2);
  // create_kthread((uint64_t)klocktest2, kernelprocess, 3);
#endif
}
void do_funny() {
  struct thread_t *fun = create_uthread(0, get_process_start(), 2);
  get_process_finish(fun->proc);
  pagemap *curpagemap = fun->proc->cur_map;
  load_elf(curpagemap, "/bin/bash", (char *[]){"/bin/ls", NULL},
           (char *[]){NULL}, &fun->arch_data.frame);
  ThreadReady(fun);
}
int scheduler_fork() {
  struct process_t *oldprocess = get_process_start();
  pagemap *new = new_pagemap();
  struct process_t *newprocess = create_process(new);
  duplicate_pagemap(oldprocess->cur_map, new);
  newprocess->cur_map = new;
  newprocess->cwd = oldprocess->cwd;
  if (oldprocess->cwdpath)
    newprocess->cwdpath = strdup(oldprocess->cwdpath);
  duplicate_process_fd(oldprocess, newprocess);
  struct thread_t *calledby = arch_get_per_cpu_data()->cur_thread;
  struct thread_t *fun = create_thread();
  fun->fpu_state = (void *)align_up(
      (uint64_t)kmalloc(align_up(get_fpu_storage_size(), 0x1000) + 64), 64);
  memcpy(fun->fpu_state, calledby->fpu_state, get_fpu_storage_size());
  fun->count = calledby->count;
  fun->tid = calledby->tid + 1;
  uint64_t kstack = (uint64_t)(kmalloc(KSTACKSIZE) + KSTACKSIZE);
  fun->kernel_stack_base = kstack;
  fun->kernel_stack_ptr = kstack;
  fun->proc = newprocess;
#if defined __x86_64__
  struct SyscallFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rcx, rbx;
  };
  struct SyscallFrame *syscallframe =
      (struct SyscallFrame
           *)(arch_get_per_cpu_data()->arch_data.kernel_stack_ptr - 12 * 8);

  fun->arch_data.frame = (struct StackFrame){};
  struct StackFrame *destframe = &fun->arch_data.frame;
  destframe->r15 = syscallframe->r15;
  destframe->r14 = syscallframe->r14;
  destframe->r13 = syscallframe->r13;
  destframe->r12 = syscallframe->r12;
  destframe->r11 = syscallframe->r11;
  destframe->r10 = syscallframe->r10;
  destframe->r9 = syscallframe->r9;
  destframe->r8 = syscallframe->r8;
  destframe->rdi = syscallframe->rdi;
  destframe->rsi = syscallframe->rsi;
  destframe->rbp = syscallframe->rbp;
  destframe->rcx = syscallframe->rcx;
  destframe->rbx = syscallframe->rbx;
  destframe->rip = syscallframe->rcx;
  // USER CODE
  destframe->cs = 0x40 | 3;
  // USER DATA
  destframe->ss = 0x38 | 3;
  destframe->rflags = syscallframe->r11;
  destframe->rsp = arch_get_per_cpu_data()->arch_data.syscall_stack_ptr_tmp;

  fun->arch_data.frame.rax = 0;
  fun->arch_data.fs_base = calledby->arch_data.fs_base;

#endif

  ThreadReady(fun);
  return fun->tid;
}
// returns the old thread
struct thread_t *switch_queue(struct per_cpu_data *cpu) {
  if (cpu->cur_thread) {
    struct thread_t *old = cpu->cur_thread;
    if (cpu->cur_thread->state == ZOMBIE || cpu->cur_thread->state == BLOCKED) {
      cpu->cur_thread = pop_from_list(&cpu->run_queue);
      cpu->cur_thread->state = RUNNING;
      return old;
    }

    cpu->cur_thread->state = READY;
    push_into_list(&cpu->run_queue, cpu->cur_thread);
    cpu->cur_thread = pop_from_list(&cpu->run_queue);
    if (cpu->cur_thread->state == READYING) {
      cpu->cur_thread->state = READY;
      push_into_list(&cpu->run_queue, cpu->cur_thread);
      cpu->cur_thread = pop_from_list(&cpu->run_queue);
      cpu->cur_thread->state = RUNNING;
      return old;
    }
    cpu->cur_thread->state = RUNNING;
    return old;
  } else {
    cpu->cur_thread = pop_from_list(&cpu->run_queue);
    cpu->cur_thread->state = RUNNING;
    return NULL;
  }
}
#ifdef __x86_64__
void load_ctx(struct StackFrame *context);
void save_ctx(struct StackFrame *dest, struct StackFrame *src);

#endif

void schedd(struct StackFrame *frame) {

  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  if (!cpu) {
    return;
  }
  if (cpu->cur_thread == NULL && cpu->run_queue == NULL) {
    // kprintf("schedd(): no threads to run\r\n");
    return;
  }
  if (cpu->cur_thread) {
    cpu->cur_thread->arch_data.fs_base = rdmsr(0xC0000100);
  }

  if (frame->cs & 3) /* if usermode */ {
#if defined(__x86_64__)
    if (cpu->cur_thread) {
      fpu_save(cpu->cur_thread->fpu_state);
    }

#endif
  }
  struct thread_t *old = switch_queue(cpu);
#if defined(__x86_64__)
  if (old != NULL) {

    save_ctx(&old->arch_data.frame, frame);
  }
  cpu->arch_data.kernel_stack_ptr = cpu->cur_thread->kernel_stack_ptr;
  arch_switch_pagemap(cpu->cur_thread->proc->cur_map);
  wrmsr(0xC0000100, cpu->cur_thread->arch_data.fs_base);
  if (cpu->cur_thread->arch_data.frame.cs & 3) /* if usermode */ {
#if defined(__x86_64__)
    fpu_store(cpu->cur_thread->fpu_state);
    change_rsp0(cpu->cur_thread->kernel_stack_base);

    __asm__ volatile("swapgs");
#endif
  }
  send_eoi();
  load_ctx(&cpu->cur_thread->arch_data.frame);
#endif
}
struct process_t *get_process_start() {
  struct per_cpu_data *cpu = arch_get_per_cpu_data();
  spinlock_lock(&cpu->cur_thread->proc->lock);
  return cpu->cur_thread->proc;
}
struct process_t *get_process_finish(struct process_t *proc) {
  spinlock_unlock(&proc->lock);
}
