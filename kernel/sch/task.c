/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[TASK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/sch.h>
#include <lib/string.h>
#include <kernel/irq.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/list.h>
#include <kernel/pid.h>
#include <kernel/spinlock.h>
#include <kernel/printk.h>
#include <kernel/timer.h>

static LIST_HEAD(task_list);
LIST_HEAD(close_task_list);
static SPINLOCK(task_list_lock);
static SPINLOCK(close_list_lock);

extern spinlock_t ready_list_lock;

static void task_exit(void)
{
    struct task_struct *task;

    task = current;
    task_del(task);
}

static void timeout(void *parameter)
{
    struct task_struct *task;

    task = (struct task_struct *)parameter;

    BUG_ON(task == NULL);
    BUG_ON(task->status != TASK_WAIT);

    spin_lock(&task->lock);
    task->flag = -ETIMEDOUT;
    if (task->list_lock) {
        spin_lock(task->list_lock);
        list_del(&task->list);
        spin_unlock(task->list_lock);
        task->list_lock = NULL;
    }
    spin_unlock(&task->lock);
    add_task_to_ready_list(task);
    switch_task();
}

static struct task_info *alloc_task_info(struct task_struct *task)
{
    struct task_info *info;

    info = (struct task_info *)alloc_page(GFP_KERNEL);
    if (info == NULL) {
        return NULL;
    }

    info->task = task;

    return info;
}

static int free_task_info(struct task_info *info)
{
    return free_page((addr_t)info);
}

static int __task_create(struct task_struct *task,
                         const char *name,
                         void (*entry)(void *parameter),
                         void *parameter,
                         addr_t *stack_start,
                         uint8_t priority,
                         uint32_t tick,
                         void (*clean)(struct task_struct *task))
{
    int rc;

    INIT_LIST_HEAD(&task->list);
    INIT_LIST_HEAD(&task->tlist);
    spin_lock_init(&task->lock);

    task->list_lock = NULL;
    task->name = name;
    task->entry = (void *)entry;
    task->parameter = parameter;
    task->stack = stack_start;
    task->sp = (addr_t *)stack_init(task->entry, task->parameter,
                                    (addr_t *)task->stack,
                                    (void *)task_exit);
    task->init_priority    = priority;
    task->current_priority = priority;
#if CONFIG_MAX_PRIORITY > 32
    task->offset = priority >> 3;
    task->offset_mask = 1UL << task->offset;
    task->prio_mask = 1UL << (priority & 0x07);
#else
    task->offset_mask = 1UL << priority;
#endif
    task->init_tick      = tick;
    task->remaining_tick = tick;
    task->status  = TASK_SUSPEND;
    task->cleanup = clean;
    task->flag = 0;
    task->start_time = 0;
    task->run_time = 0;
    task->sys_cycle = 0;
    task->save_sys_cycle = 0;
    task->save_run_time = 0;

    spin_lock(&task_list_lock);
    list_add_tail(&task->tlist, &task_list);
    spin_unlock(&task_list_lock);
    rc = timer_init(&task->timer, task->name, timeout, task);
    if (rc < 0) {
        pr_err("%s init timer error, rc=%d\r\n", task->name, rc);
    }

    return rc;
}

struct task_struct *task_create(const char *name,
                                void (*entry)(void *parameter),
                                void *parameter,
                                uint8_t priority,
                                uint32_t tick,
                                void (*clean)(struct task_struct *task))
{
    struct task_struct *task;
    addr_t *stack_start;
    pid_t pid;
    struct task_info *info;
    int rc;

#if CONFIG_MAX_PRIORITY < 256
    if (priority >= CONFIG_MAX_PRIORITY) {
        pr_err("%s: Priority should be less than %d\r\n", name, CONFIG_MAX_PRIORITY);
        return NULL;
    }
#endif

    task = kalloc(sizeof(struct task_struct), GFP_KERNEL);
    if (task == NULL) {
        pr_err("%s: alloc task struct buf error\r\n", name);
        goto task_struct_err;
    }

    info = alloc_task_info(task);
    if (info == NULL) {
        pr_err("%s: alloc task info buf error\r\n", name);
        goto task_info_err;
    }
#ifdef CONFIG_STACK_GROWSUP
    stack_start = (addr_t *)((addr_t)info + sizeof(struct task_info));
#else
    stack_start = (addr_t *)((addr_t)info + sizeof(union task_union) - sizeof(addr_t));
#endif

    pid = pid_alloc();
    if (!pid) {
        pr_err("%s: alloc pid error\r\n", name);
        goto pid_err;
    }

    task->pid = pid;

    rc = __task_create(task, name, entry, parameter, stack_start, priority, tick, clean);
    if (rc < 0) {
        pr_err("%s: task_create error, rc=%d\r\n", name, rc);
        goto task_create_err;
    }

    return task;

task_create_err:
    pid_free(pid);
pid_err:
    free_task_info(info);
task_info_err:
    kfree(task);
task_struct_err:
    return NULL;
}

int task_ready(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }

    if (task->status == TASK_READY || task->status == TASK_RUNING) {
        pr_warning("%s:task is already in a ready state\r\n", task->name);
        return -EINVAL;
    }

    add_task_to_ready_list(task);
    switch_task();

    return 0;
}

int task_yield_cpu(void)
{
    struct task_struct *task;

    task = current;
    if (task->status == TASK_RUNING) {
        del_task_to_ready_list(task);
        add_task_to_ready_list(task);
        switch_task();
    } else {
        BUG_ON(true);
    }

    return 0;
}

void task_del(struct task_struct *task)
{
   if (!task) {
        pr_err("task struct is NULL\r\n");
        return;
    }

    if (task->status == TASK_READY || task->status == TASK_RUNING) {
        del_task_to_ready_list(task);
    }

    spin_lock(&task->lock);
    spin_lock(&task_list_lock);
    list_del(&task->tlist);
    spin_unlock(&task_list_lock);
    task->status = TASK_CLOSE;
    timer_stop(&task->timer);
    task->list_lock = &close_list_lock;
    spin_lock(task->list_lock);
    list_add_tail(&task->list, &close_task_list);
    spin_unlock(task->list_lock);
    spin_unlock(&task->lock);

    if (task == current) {
        switch_task();
    }
}

int task_hang(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }
    if (task->status != TASK_READY && task->status != TASK_RUNING) {
        pr_err("task status is not TASK_READY or TASK_RUNING\r\n");
        if (task == current) {
            BUG_ON(true);
        }
        return -EINVAL;
    }

    timer_stop(&task->timer);
    del_task_to_ready_list(task);
    spin_lock(&task->lock);
    task->status = TASK_WAIT;
    spin_unlock(&task->lock);

    return 0;
}

int task_resume(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }
    if (task->status != TASK_WAIT) {
        pr_err("task(=%s) status(=%d) is not TASK_WAIT\r\n", task->name, task->status);
        BUG_ON(true);
        return -EINVAL;
    }

    timer_stop(&task->timer);
    add_task_to_ready_list(task);

    return 0;
}

int task_sleep(u32 tick)
{
    struct task_struct *task;
    int rc;

    if (!tick) {
        pr_err("sleep tick is 0\r\n");
        return -EINVAL;
    }

    task = current;
    rc = task_hang(task);
    if (rc) {
        pr_err("%s task hang error, rc=%d\r\n", task->name, rc);
        return rc;
    }
    timer_start(&task->timer, tick);
    switch_task();

    if (task->flag == -ETIMEDOUT)
        task->flag = 0;

    return 0;
}

int task_set_prio(struct task_struct *task, uint8_t prio)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return -EINVAL;
    }

    if (task->current_priority == prio) {
        return 0;
    }

    del_task_to_ready_list(task);
    spin_lock(&task->lock);
    task->current_priority = prio;
    spin_unlock(&task->lock);
    add_task_to_ready_list(task);

    return 0;
}
