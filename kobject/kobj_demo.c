/*
* 创建时间:2026/10/14
* 创建作者:houchao
* 功能说明:1.说明kobject_init_and_add内核接口的功能,即向内核创建并新插入一个抽象的内核对象kobject
*            同时定义kobj type
*          2.内核对象kobject是嵌入到用户自定义结构体中的,可以通过sysfs语义对用户自定义结构体进行操作
*          3.采用kobject的好处是:
*            a.统一命名,在/sys/kernel下生成树状结构
*            b.统一生命周期,通过kobj type自定义函数, 通过引用计数进行管理对象的生命周期
*            c.统一和用户态的交互
*/ 

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/string.h>

#define DEMO_NAME "kobj_demo"

// 声明一个结构体
struct my_kobj_demo
{
    struct kobject kobj;
    int value;
    char name[32];
};

#define to_my_demo(k) container_of(k, struct my_kobj_demo, kobj)

static ssize_t value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct my_kobj_demo *p_demo = to_my_demo(kobj);
    return scnprintf(buf, PAGE_SIZE, "%d\n", p_demo->value);
}

static ssize_t value_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    struct my_kobj_demo *p_demo = to_my_demo(kobj);
    int ret                     = 0;

    ret = kstrtoint(buf, 10, &p_demo->value);
    if (ret)
    {
        goto l_ret;
    }

    pr_info(DEMO_NAME ": value set to %d\n", p_demo->value);
    return count;

l_ret:
    return ret;
}

// 属性定义,定义权限和回调
static struct kobj_attribute value_attr = __ATTR(value, 0644, value_show, value_store);

// 属性数组
static struct attribute *my_demo_attrs[] =
{
    &value_attr.attr,
    NULL,
};

static void my_demo_release(struct kobject *kobj)
{
    struct my_kobj_demo *p_demo = to_my_demo(kobj);
    pr_info(DEMO_NAME ": release called, freeing %s\n", p_demo->name);
    kfree(p_demo);
}

static struct kobj_type my_demo_ktype = {
    .release      = my_demo_release,
    .sysfs_ops    = &kobj_sysfs_ops,
    .default_attrs = my_demo_attrs,
};

static struct my_kobj_demo *g_demo;

/*
函数功能:内核ko初始化
函数入参:
创建时间:2026/10/13
创建作者:houchao修改
*/
static int kobj_demo_init(void)
{
    struct my_kobj_demo *p_demo = NULL;
    int ret                     = 0;

    pr_info(DEMO_NAME "module init\n");

    p_demo = kzalloc(sizeof(*p_demo), GFP_KERNEL);
    if (unlikely(NULL == p_demo))
    {
        ret = -ENOMEM;
        goto l_out;
    }

    p_demo->value = 42;
    strlcpy(p_demo->name, DEMO_NAME, sizeof(p_demo->name));

    ret = kobject_init_and_add(&p_demo->kobj, &my_demo_ktype, kernel_kobj, "%s", DEMO_NAME);
    if (ret)
    {
        pr_err(DEMO_NAME "kobject_init_and_add failed: %d\n", ret);
        kobject_put(&p_demo->kobj);
        goto l_out;
    }
    g_demo = p_demo;

    pr_info(DEMO_NAME "created at /sys/kernel/%s (value=%d)\n", DEMO_NAME, p_demo->value);

l_out:
    return ret;

}

/*
函数功能:内核ko退出
函数入参:
创建时间:2026/10/13
创建作者:houchao修改
*/
static void kobj_demo_exit(void)
{
    pr_info(DEMO_NAME "module exit\n");

    if (g_demo)
    {
        kobject_put(&g_demo->kobj);
        g_demo = NULL;
    }
}

module_init(kobj_demo_init);
module_exit(kobj_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Demo");
MODULE_DESCRIPTION("Demo for kobject_init_and_add (3.10 kernel)");
