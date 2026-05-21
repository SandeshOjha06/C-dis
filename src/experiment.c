// experiment.c
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char c;
    int  n;
    while (1) {
        n = read(STDIN_FILENO, &c, 1);
        if (n == 0) break;
        printf("got byte: '%c' (0x%02x)\n", c, (unsigned char)c);
    }
    return 0;
}