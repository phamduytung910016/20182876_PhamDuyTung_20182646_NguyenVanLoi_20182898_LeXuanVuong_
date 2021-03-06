#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/frame.h"
#include "vm/page.h"

#define MAX_ARGS_SIZE 4096

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static int set_up_user_prog_stack (void **esp, char **save_ptr, char *token);
 
/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *fn_copy_2;
  char *save_ptr;
  tid_t tid;
  struct thread *t;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Use malloc as we only need a few bytes, not a whole page. */
  //cấp phát bộ nhớ động
  fn_copy_2 = malloc ( strlen(file_name) + 1);
  if (fn_copy_2 == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
    
  strlcpy (fn_copy_2, file_name, PGSIZE);
  file_name = strtok_r (fn_copy_2, " ", &save_ptr);//tách để lấy tham số đầu tiên là tên file 
  
  /*
  Ví dụ : truyền "echo x y z" -> file_name = echo , *save_ptr sẽ lưu vị trí x
  */

  tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
  //tạo luồng với tên file là giá trị vừa tách
  free (fn_copy_2);
    
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      return tid;
    }

  t = thread_by_tid (tid);
  sema_down (&t->sema_wait);
  if (t->ret_status == RET_STATUS_ERROR)
    tid = TID_ERROR;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;
  struct thread *cur;
  char *save_ptr;
  char *token;
 
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Extract file name. */
  token = strtok_r (file_name, " ", &save_ptr);
  //token = "echo"
  

  success = load (file_name, &if_.eip, &if_.esp);
  /*
  Hàm load sẽ dùng để tìm file có thể tiến hành được, sử dụng tên file và tải file từ disk vào bộ nhớ .
  3 tham số vào hàm load : tên chương trình , Function entry point, stack top pointer (user stack): con trỏ đầu ngăn xếp người dùng
  */
  

  cur = thread_current();//lấy ra luồng đang chạy
  
  if (success) 
  {
    //Tiếp tục execute file người dùng
    /* Set up the stack for the user program. */
    set_up_user_prog_stack (&if_.esp, &save_ptr, token);//cài đặt ngăn xếp cho chương trình người dùng

    cur->exec = filesys_open (file_name);//mở file
    file_deny_write ( cur->exec );
    sema_up (&cur->sema_wait);
  }
  else
  {
    /* If load failed, quit. */
    palloc_free_page (file_name);

    cur->ret_status = RET_STATUS_ERROR;
    sema_up (&cur->sema_wait);
    thread_exit ();
  }

  /* Free memory */
  palloc_free_page (file_name);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  // vào chế độ người dùng để thực hiện bất kỳ chương rình nào
  NOT_REACHED ();
}

/* Sets up the stack for the user program, returns 1 on success, 0 if the
   arguments won't fit on the stack */
//Tạo ngăn xếp cho chương trình người dùng, trả về giá trị 1 nếu thành công, 0 nếu
//đối số không vừa với ngăn xếp    
static int
set_up_user_prog_stack (void **esp, char **save_ptr, char* token) {
  int args_pushed;
  int argc = 0;
  void* stack_pointer;

//con trỏ ngăn xếp = giá trị mà thanh ghi vừa lưu khi ngắt
  stack_pointer = *esp;

  /* Tokenise file name and push each token on the stack. */
  //đẩy từng đối số vào ngăn xếp
  do                                                                            
     {                                                                           
       size_t len = strlen (token) + 1;                                          
       stack_pointer = (void*) (((char*) stack_pointer) - len);                  
       strlcpy ((char*)stack_pointer, token, len);                               
       argc++;                                   
       /* Don't push anymore arguments if maximum allowed 
          have already been pushed. */
       if (PHYS_BASE - stack_pointer > MAX_ARGS_SIZE)
          return 0;                              
       token = strtok_r (NULL, " ", save_ptr);
       //token = x                                  
     } while (token != NULL);
  
  char *arg_ptr = (char*) stack_pointer;                                      
  
  /* Round stack pionter down to a multiple of 4. */
  //Làm tròn con trỏ xuống bội số của 4
  stack_pointer = (void*) (((intptr_t) stack_pointer) & 0xfffffffc);

  /* Push null sentinel. */
  //đưa null sentinel vào 
  stack_pointer = (((char**) stack_pointer) - 1);
  *((char*)(stack_pointer)) = 0;

  /* Push pointers to arguments. */
  //đẩy con trỏ trỏ tới các đối số theo thứ tự ngược lại
  args_pushed = 0;                                                              
  while (args_pushed < argc)                                                    
     {                                                                           
       while (*(arg_ptr - 1) != '\0')                                            
         ++arg_ptr;                                                              
       stack_pointer = (((char**) stack_pointer) - 1);                           
       *((char**) stack_pointer) = arg_ptr;                                      
       ++args_pushed;    
       ++arg_ptr;                                                        
     }

  /* Push argv. */
  //đẩy argv
  char** first_arg_pointer = (char**) stack_pointer;
  stack_pointer = (((char**) stack_pointer) - 1);
  *((char***) stack_pointer) = first_arg_pointer;


  /* Push argc. */
  //đẩy argc
  int* stack_int_pointer = (int*) stack_pointer;
  --stack_int_pointer;
  *stack_int_pointer = argc;
  stack_pointer = (void*) stack_int_pointer;

  /* Push null sentinel. */
  //đẩy null sentinel
  stack_pointer = (((void**) stack_pointer) - 1);
  *((void**)(stack_pointer)) = 0;

  *esp = stack_pointer;
  return 1;
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *t, *cur;

  cur = thread_current ();
  t = thread_by_tid (child_tid);
  
  if (t == NULL || t->parent != cur || t->waited)
    return RET_STATUS_ERROR;
  else if (t->ret_status != RET_STATUS_INIT || t->exited == true)
    return t->ret_status;
 
  sema_down (&t->sema_wait);
  int ret = t->ret_status;
  sema_up (&t->sema_exit);
  t->waited = true;

  return ret;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
    
  printf ("%s: exit(%d)\n", cur->name, cur->ret_status);

  if (cur->exec != NULL)
    file_allow_write (cur->exec);

  while (!list_empty (&cur->sema_wait.waiters))
    sema_up (&cur->sema_wait);
  
  cur->exited = true;
  if (cur->parent != NULL)
    sema_down (&cur->sema_exit); 

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

int test = 0;

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  off_t load_ofs = ofs;
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      //block_sector_t sector_idx = inode_get_inumber (file_get_inode (file), load_ofs);
      off_t block_id = -1;

      /* If we have a read-only segment obtain its corresponding block sector 
         to be used later on in sharing read-only frames. */
      if (writable == false)
        block_id = inode_get_block_number (file_get_inode (file), load_ofs);

      //printf ("[Load segemnt] rb=%d zb=%d writable=%d page=%d\n", page_read_bytes, page_zero_bytes, writable, upage);
      //printf ("Inode: %d Ofs %d\n", block_id, load_ofs);

      struct vm_page *page = NULL;
      page = vm_new_file_page (upage, file, load_ofs, page_read_bytes, 
                                page_zero_bytes, writable, block_id);
      if (page == NULL)
        return false;

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      load_ofs += PGSIZE;
    }
  file_seek (file, ofs);

  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct vm_page *page = NULL;

  page = vm_new_zero_page (((uint8_t *) PHYS_BASE) - PGSIZE, true); 
  if (page == NULL)
    return false;
  
  *esp = PHYS_BASE;
  vm_load_page (page, false);  

  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
#ifndef VM
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
#endif
