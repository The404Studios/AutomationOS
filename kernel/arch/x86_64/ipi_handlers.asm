; IPI (Inter-Processor Interrupt) Handlers
; =========================================
;
; Low-level interrupt handlers for IPI vectors
; These call the C handlers in ipi.c

[BITS 64]

extern ipi_handle_reschedule
extern ipi_handle_tlb_flush
extern ipi_handle_function_call
extern ipi_handle_stop

global ipi_reschedule_handler
global ipi_tlb_flush_handler
global ipi_function_call_handler
global ipi_stop_handler

; Common IPI handler prologue
%macro IPI_HANDLER_PROLOGUE 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

; Common IPI handler epilogue
%macro IPI_HANDLER_EPILOGUE 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq
%endmacro

; IPI_RESCHEDULE handler (vector 0x40)
ipi_reschedule_handler:
    IPI_HANDLER_PROLOGUE
    call ipi_handle_reschedule
    IPI_HANDLER_EPILOGUE

; IPI_TLB_FLUSH handler (vector 0x41)
ipi_tlb_flush_handler:
    IPI_HANDLER_PROLOGUE
    call ipi_handle_tlb_flush
    IPI_HANDLER_EPILOGUE

; IPI_FUNCTION_CALL handler (vector 0x42)
ipi_function_call_handler:
    IPI_HANDLER_PROLOGUE
    call ipi_handle_function_call
    IPI_HANDLER_EPILOGUE

; IPI_STOP handler (vector 0x43)
ipi_stop_handler:
    IPI_HANDLER_PROLOGUE
    call ipi_handle_stop
    ; Never returns
    cli
.halt:
    hlt
    jmp .halt
