#include "sched.h"

#include <mem/kmem.h>
#include <stdint.h>

#include "arch/arch.h"
#include "arch/x86_64/cpu/lapic.h"
#include "arch/x86_64/instructions/instructions.h"
#include "mem/vmm.h"
#include "sched/reaper.h"
#include "smp/smp.h"
#include "utils/basic.h"
// ARCH STUFF
#include "arch/x86_64/cpu/structures.h"

#ifdef __x86_64__
extern void do_savekstackandloadkstack(struct thread_t* old, struct thread_t* new);
#endif
void arch_save_ctx(void* frame, struct thread_t* threadtosavectx)
{
#ifdef __x86_64__
	// threadtosavectx->arch_data.frame = *(struct StackFrame*)frame;
	//__asm__ volatile("mov %%rsp, %0" : : "r"(threadtosavectx->kernel_stack_ptr));
#endif
}
void arch_load_ctx(void* frame, struct thread_t* threadtoloadctxfrom)
{
#ifdef __x86_64__
	// *(struct StackFrame*)frame = threadtoloadctxfrom->arch_data.frame;

	//__asm__ volatile("mov %0, %%rsp" : : "r"(threadtoloadctxfrom->kernel_stack_ptr));
#endif
}
#if defined(__x86_64__)
struct per_cpu_data bsp = {.run_queue = NULL};
#endif
struct per_cpu_data* arch_get_per_cpu_data()
{
#if defined(__x86_64__)
	if (get_lapic_id() == bsp_id)
	{
		assert(&bsp != NULL);
		return &bsp;
	}
	struct per_cpu_data* hi = (struct per_cpu_data*)rdmsr(0xC0000101);
	return hi;
#endif
}
void arch_create_per_cpu_data()
{
#if defined(__x86_64__)
	struct per_cpu_data* hey = (struct per_cpu_data*)kmalloc(sizeof(struct per_cpu_data));
	hey->run_queue = NULL;
	hey->arch_data.lapic_id = get_lapic_id();
	hey->arch_data.kernel_stack_ptr = 0;
	hey->arch_data.syscall_stack_ptr_tmp = 0;
	wrmsr(0xC0000101, (uint64_t)hey);
#endif
}
struct process_t* create_process(pagemap* map)
{
	struct process_t* him = (struct process_t*)kmalloc(sizeof(struct process_t));
	him->cur_map = map;
	him->lock = SPINLOCK_INITIALIZER;
	him->cnt = 0;
	return him;
}
struct thread_t* create_thread()
{
	struct thread_t* him = (struct thread_t*)kmalloc(sizeof(struct thread_t));
	// him->next = NULL;
	return him;
}
void push_into_list(struct thread_t** list, struct thread_t* whatuwannapush) {
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
struct thread_t *pop_from_list(struct thread_t** list) {
    struct thread_t *old = *list;
    *list = (*list)->next;
    (*list)->back = old->back;
    (*list)->back->next = *list;
    return old;
}
#if defined(__x86_64)
extern void return_from_kernel_in_new_thread();
void load_ctx_into_kstack(struct thread_t* t, struct StackFrame usrctx)
{
	t->kernel_stack_ptr -= sizeof(struct StackFrame);
	*(struct StackFrame*)t->kernel_stack_ptr = usrctx;
	t->kernel_stack_ptr -= sizeof(uint64_t*);
	*(uint64_t*)t->kernel_stack_ptr = (uint64_t)return_from_kernel_in_new_thread;
	t->kernel_stack_ptr -= sizeof(uint64_t) * 7;
}
#endif
void exit_thread()
{
	__asm__ volatile ("cli");
	kprintf("exit thread is a stub\r\n");
	struct per_cpu_data* cpu = arch_get_per_cpu_data();
	cpu->cur_thread->state = ZOMBIE;

	push_into_list(&cpu->to_be_reapered, cpu->cur_thread);
	kprintf("pushed into list\r\n");
	refcount_dec(&cpu->to_be_reapered->proc->cnt);
	refcount_dec(&cpu->to_be_reapered->count);
	refcount_dec(&cpu->to_be_reapered->count);
	// cpu->cur_thread = NULL;
	// no need to assert
	// reaper thread is always running
	schedd(NULL);
}
void ThreadBlock(struct thread_t *whichqueue) {
	struct per_cpu_data* cpu = arch_get_per_cpu_data();
	cpu->cur_thread->state = BLOCKED;
	push_into_list(&whichqueue, cpu->cur_thread);
	schedd(NULL);
}
void ThreadReady(struct thread_t *thread) {
	struct per_cpu_data* cpu = arch_get_per_cpu_data();
	thread->state = READY;
	push_into_list(&cpu->run_queue, thread);
}
void create_kentry()
{
#if defined(__x86_64__)
	struct process_t* h = create_process(&ker_map);
	struct thread_t* e = create_thread();
	e->proc = h;
	refcount_inc(&h->cnt);
	// 16kib kstack lol
	uint64_t kstack = (uint64_t)(kmalloc(KSTACKSIZE) + KSTACKSIZE);	   // top of stack
	// profiler will crash because it expects a return address
	struct StackFrame hh = arch_create_frame(false, (uint64_t)kentry, kstack - 8);
	e->kernel_stack_base = kstack;
	e->kernel_stack_ptr = kstack;
	e->arch_data.frame = hh;
	load_ctx_into_kstack(e, hh);
	// kprintf("ran fine\r\n");
	struct per_cpu_data* cpu = arch_get_per_cpu_data();

	// we are not done tho we need to create the reaper thread
	struct process_t* reaperprocess = create_process(&ker_map);
	struct thread_t* reaperthread = create_thread();
	reaperthread->proc = reaperprocess;
	uint64_t kstackforreaper = (uint64_t)(kmalloc(KSTACKSIZE) + KSTACKSIZE);
	struct StackFrame reaperframe = arch_create_frame(false, (uint64_t)reaper, kstackforreaper - 8);
	reaperthread->kernel_stack_base = kstackforreaper;
	reaperthread->kernel_stack_ptr = kstackforreaper;
	load_ctx_into_kstack(reaperthread, reaperframe);
	reaperthread->back = e;
	reaperthread->next = e;
	reaperthread->tid = 0;
	e->next = reaperthread;
	e->back = reaperthread;
	e->tid = 1;
	cpu->run_queue = reaperthread;
	refcount_inc(&e->count);
	refcount_inc(&e->count);
	refcount_inc(&reaperthread->count);
	refcount_inc(&reaperthread->count);
	// e->back = e;
	// e->next = e;
	// cpu->run_queue = e;
	// assert(cpu->run_queue->next != NULL);
	// refcount_inc(&e->count);

#endif
}
// returns the old thread
struct thread_t* switch_queue(struct per_cpu_data* cpu)
{
	if (cpu->cur_thread)
	{
		struct thread_t *old = cpu->cur_thread;
		if (cpu->cur_thread->state == ZOMBIE || cpu->cur_thread->state == BLOCKED) {
			kprintf("switch_queue(): thread is blocked or zombie\r\n");
			cpu->cur_thread = pop_from_list(&cpu->run_queue);
		cpu->cur_thread->state = RUNNING;
		kprintf("switch_queue(): switching to tid %d\r\n", cpu->cur_thread->tid);
		return old;
		} 
		cpu->cur_thread->state = READY;
		push_into_list(&cpu->run_queue, cpu->cur_thread);
		cpu->cur_thread = pop_from_list(&cpu->run_queue);
		cpu->cur_thread->state = RUNNING;
		return old;
		
	} else {
		cpu->cur_thread = pop_from_list(&cpu->run_queue);
		cpu->cur_thread->state = RUNNING;
		return NULL;
	}
}

void schedd(void* frame)
{
	struct per_cpu_data* cpu = arch_get_per_cpu_data();
	if (cpu->cur_thread == NULL && cpu->run_queue == NULL)
	{
		//kprintf("schedd(): no threads to run\r\n");
		return;
	}
	struct thread_t* old = switch_queue(cpu);
#if defined(__x86_64__)
	arch_switch_pagemap(cpu->cur_thread->proc->cur_map);
	send_eoi();
	do_savekstackandloadkstack(old, cpu->cur_thread);
#endif
	// arch_load_ctx(frame, cpu->run_queue);
	//  for reading operations such as switching the pagemap
	//  it is fine not to lock, otherwise we would have deadlocks and major slowdowns
	//  for writing anything however, the process MUST be locked
	// we dont ever return
}
