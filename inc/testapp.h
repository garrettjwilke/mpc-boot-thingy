#ifndef TESTAPP_H
#define TESTAPP_H

/* Stock /testapp asks the menu to release the screen through a request file.
   MPC_BOOT_TESTAPP_REQUEST and MPC_BOOT_TESTAPP_READY override the paths. */

/* 1 when the wrapper has asked us to release the screen. */
int testapp_request_pending(void);

/* Tell the wrapper the screen and pads are free. 0 on success. */
int testapp_mark_ready(void);

/* 1 when comm is test-app-launcher or usb_installer, including the 15-byte
   task name the kernel stores. */
int testapp_comm_matches(const char *comm);

/* 1 when proc_root has a process whose comm matches. */
int testapp_proc_has_launcher(const char *proc_root);

#endif
