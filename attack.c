// bias_attack.c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd_in = open("/tmp/sensor_raw", O_RDONLY);
    int fd_out = open("/tmp/sensor_spoofed", O_WRONLY);
    float value;
    float bias = 10.0;

    while (read(fd_in, &value, sizeof(float)) > 0) {
        value += bias; 
        
        usleep(500000); // delay 0.5s

        write(fd_out, &value, sizeof(float));
    }
    return 0;
}
