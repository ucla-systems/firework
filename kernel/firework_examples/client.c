#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main() {
    int ret = syscall(451);
    printf("System call returned %d\n", ret);
    return 0;
}