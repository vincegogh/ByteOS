section .text
global long_mode_start
long_mode_start:
	mov ax, 0x10
	mov ss, ax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

	push rbx ; Multiboot structure (physical address)

	; TODO: Enable AVX

	; Call global constructors
	extern _init
	call _init

	; Initialise VGA textmode driver
	extern vga_tmode_init
	call vga_tmode_init

	; Load interrupt descriptor table
	extern load_idt
	call load_idt
	
	; Pass multiboot information to kmain
	pop rdi
	extern kmain
	call kmain

	; Call global destructors
	extern _fini
	call _fini

	sti
.end:
	hlt
	jmp .end
