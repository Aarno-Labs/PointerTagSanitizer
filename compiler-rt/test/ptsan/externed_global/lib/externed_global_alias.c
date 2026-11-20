char secret_buf[20];
extern char buf[20] __attribute__((alias("secret_buf")));