#ifndef KERNEL_API_H
#define KERNEL_API_H
/* Constants shared by the kernel_*.c files. */

/* WF_PMODE | WF_CPU486 | WF_ENHANCED | WF_PAGING; no 80x87 reported. */
#define KERNEL_WINFLAGS 0x0C29 /* PMODE | ENHANCED | CPU486 | 80x87 | PAGING */

#endif
