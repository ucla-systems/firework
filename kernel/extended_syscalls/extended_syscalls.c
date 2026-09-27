#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/tcp.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/in.h>
#include <linux/firework.h>

extern uint16_t firework_default16;
extern uint32_t firework_default32;

int read_sockinfo_from_debugfs(struct sockaddr_in *server_addr) {
    // struct file *file;
    // loff_t pos = 0;
    // uint32_t ip_addr;
    // uint16_t port;

    // file = filp_open("/sys/kernel/debug/firework/ipaddr", O_RDONLY, 0);
    // if (IS_ERR(file)) {
    //     printk(KERN_ERR "Error opening ipaddr file\n");
    //     return -1;
    // }
    // if (kernel_read(file, &ip_addr, sizeof(ip_addr), &pos) != sizeof(ip_addr)) {
    //     printk(KERN_ERR "Error reading ipaddr file\n");
    //     filp_close(file, NULL);
    //     return -1;
    // }
    // filp_close(file, NULL);

    // pos = 0;
    // file = filp_open("/sys/kernel/debug/firework/port", O_RDONLY, 0);
    // if (IS_ERR(file)) {
    //     printk(KERN_ERR "Error opening port file\n");
    //     return -1;
    // }

    // if (kernel_read(file, &port, sizeof(port), &pos) != sizeof(port)) {
    //     printk(KERN_ERR "Error reading port file\n");
    //     filp_close(file, NULL);
    //     return -1;
    // }
    // filp_close(file, NULL);

    server_addr->sin_addr.s_addr = htonl(firework_default32);
    server_addr->sin_port = htons(firework_default16);

    return 0;
}


SYSCALL_DEFINE0(sharing_request)
{
    struct socket *sock;
    struct sockaddr_in server_addr;
    char *message = "Hello from client in the kernel!";
    struct msghdr msg;
    struct kvec iov;

    int err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (err < 0) {
        printk(KERN_ERR "Error creating socket\n");
        return err;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    err = read_sockinfo_from_debugfs(&server_addr);
    if (err < 0) {
        printk(KERN_ERR "Error reading server info\n");
        sock_release(sock);
        return err;
    }
    

    err = sock->ops->connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr), 0);
    if (err < 0) {
        printk(KERN_ERR "Error connecting to server\n");
        sock_release(sock);
        return err;
    }

    iov.iov_base = message;
    iov.iov_len = strlen(message);
    memset(&msg, 0, sizeof(msg));

    err = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
    if (err < 0) {
        printk(KERN_ERR "[firework: sharing_request] Error sending message\n");
    } else {
        printk(KERN_INFO "[firework: sharing_request] Message sent\n");
    }

    sock_release(sock);

    return 0;
}

SYSCALL_DEFINE2(shared_memory_init, size_t, sharing_map_size, size_t, queue_capacity)
{
    return shared_mem_init(sharing_map_size, queue_capacity);
}

SYSCALL_DEFINE3(shared_memory_put, int, host_idx, uint64_t, addr, unsigned long, pfn)
{
    return shared_mem_put(host_idx, addr, pfn);
}

SYSCALL_DEFINE2(shared_memory_get, int, host_idx, uint64_t, addr)
{
    return shared_mem_get(host_idx, addr);
}

SYSCALL_DEFINE1(shared_memory_get_or_create, uint64_t, addr)
{
    int status;
    uint8_t num_pages;
    uint32_t recheck_hash_idx;
    int owner_node;
    return shared_mem_get_or_create(addr, &status, &num_pages, &recheck_hash_idx, &owner_node, NULL);
}

SYSCALL_DEFINE0(shared_memory_exit)
{
    shared_mem_exit();
    return 0;
}

SYSCALL_DEFINE2(firework_nw_init, int, fw_server_idx, int, fw_server_num)
{
    return firework_network_init(fw_server_idx, fw_server_num);
}

SYSCALL_DEFINE1(firework_unmap_page, const void __user *, addr)
{
    // return unsharing_unmap(addr);
    return 0;
}

SYSCALL_DEFINE3(firework_set_status_flag, uint64_t, addr, int, mode, int, idx)
{
    return set_status_flag(addr, mode, idx);
}

SYSCALL_DEFINE0(firework_unsharing_check) {
    return monitor_hwc();
}

SYSCALL_DEFINE0(firework_check_done_unmap) {
    return check_done_unmap();
}

SYSCALL_DEFINE0(firework_scan_shared_pages) {
    scan_shared_pages();
    return 0;
}

SYSCALL_DEFINE0(firework_shared_page_count) {
    shared_page_count();
    return 0;
}

SYSCALL_DEFINE0(firework_enable_unsharing) {
    enable_unsharing();
    return 0;
}

SYSCALL_DEFINE0(firework_do_unsharing_all) {
    do_unsharing_all();
    return 0;
}