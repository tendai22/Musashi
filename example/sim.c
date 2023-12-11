//
// sim for emu68kplus Single Board Computer
// 2023-5-20 Norihiro Kumagai
//
// This system is delived from 'Musashi' 68000 emulator
// The file 'sim.c' is also derived from it.
//

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sim.h"
#include "m68k.h"
#include "osd.h"

void disassemble_program();

/* Memory-mapped IO ports */
// adapted to emu68kplus
#define INPUT_ADDRESS 0x800000
#define OUTPUT_ADDRESS 0x400000

#define UART_DREG_ADDRESS 0x800A0	// data register
#define UART_CREG_ADDRESS 0x800A1	// command/status register

/* IRQ connections */
#define IRQ_NMI_DEVICE 7
#define IRQ_INPUT_DEVICE 2
#define IRQ_OUTPUT_DEVICE 1

/* Time between characters sent to output device (seconds) */
// changed as milliseconds about 10000bps serial speed
#define OUTPUT_DEVICE_PERIOD 1

/* ROM and RAM sizes */
//#define MAX_ROM 0xfff
// emu68kplus has 128kByte RAM
#define MAX_RAM 0x1ffff


/* Read/write macros */
#define READ_BYTE(BASE, ADDR) (BASE)[ADDR]
#define READ_WORD(BASE, ADDR) (((BASE)[ADDR]<<8) |			\
							  (BASE)[(ADDR)+1])
#define READ_LONG(BASE, ADDR) (((BASE)[ADDR]<<24) |			\
							  ((BASE)[(ADDR)+1]<<16) |		\
							  ((BASE)[(ADDR)+2]<<8) |		\
							  (BASE)[(ADDR)+3])

#define WRITE_BYTE(BASE, ADDR, VAL) (BASE)[ADDR] = (VAL)&0xff
#define WRITE_WORD(BASE, ADDR, VAL) (BASE)[ADDR] = ((VAL)>>8) & 0xff;		\
									(BASE)[(ADDR)+1] = (VAL)&0xff
#define WRITE_LONG(BASE, ADDR, VAL) (BASE)[ADDR] = ((VAL)>>24) & 0xff;		\
									(BASE)[(ADDR)+1] = ((VAL)>>16)&0xff;	\
									(BASE)[(ADDR)+2] = ((VAL)>>8)&0xff;		\
									(BASE)[(ADDR)+3] = (VAL)&0xff


/* Prototypes */
void exit_error(char* fmt, ...);

unsigned int cpu_read_byte(unsigned int address);
unsigned int cpu_read_word(unsigned int address);
unsigned int cpu_read_long(unsigned int address);
void cpu_write_byte(unsigned int address, unsigned int value);
void cpu_write_word(unsigned int address, unsigned int value);
void cpu_write_long(unsigned int address, unsigned int value);
void cpu_pulse_reset(void);
void cpu_set_fc(unsigned int fc);
int cpu_irq_ack(int level);

void nmi_device_reset(void);
void nmi_device_update(void);
int nmi_device_ack(void);

void input_device_reset(void);
void input_device_update(void);
int input_device_ack(void);
unsigned int input_device_read(void);
unsigned int input_device_status(void);
void input_device_write(unsigned int value);

void output_device_reset(void);
void output_device_update(void);
int output_device_ack(void);
unsigned int output_device_read(void);
void output_device_write(unsigned int value);
unsigned int debug_port_read(unsigned int addr);
void debug_port_write(unsigned int addr, unsigned int value);

void int_controller_set(unsigned int value);
void int_controller_clear(unsigned int value);

void update_user_input(void);

int uart_creg_read(void);
int uart_dreg_read(void);
void uart_dreg_write(unsigned char c);

void xprintf(const char *format, ...);
void xgets(char *buf, int size, int noecho_flag);


/* Data */
unsigned int g_quit = 0;                        /* 1 if we want to quit */
unsigned int g_nmi = 0;                         /* 1 if nmi pending */

// one byte read ahead and ungetc
// we compose PIR9 (uart creg) with g_input_device_ready 
// and g_output_device_ready
int		g_input_device_value = -1;
int		g_input_device_ready = 0;			/* Current status in input device */

int		g_output_device_data_ready = 0;		/* 1 if g_output_device_data is valid, to be sent */
int		g_output_device_data = 0xe5;		/* output data to be sent, 0xe5 has no means, magic number */
int		g_output_device_empty = 1;			/* 1 if output queue is empty, ready to be written to DREG */
time_t	g_output_device_last_output;		/* Time of last char output */

unsigned int g_int_controller_pending = 0;      /* list of pending interrupts */
unsigned int g_int_controller_highest_int = 0;  /* Highest pending interrupt */
#if defined(MAX_ROM)
unsigned char g_rom[MAX_ROM+1];                 /* ROM */
#endif //MAX_ROM
unsigned char g_ram[MAX_RAM+1];                 /* RAM */
unsigned int  g_fc;                             /* Current function code from CPU */


/* Exit with an error message.  Use printf syntax. */
void exit_error(char* fmt, ...)
{
	static int guard_val = 0;
	char buff[100];
	unsigned int pc;
	va_list args;

	if(guard_val)
		return;
	else
		guard_val = 1;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	pc = m68k_get_reg(NULL, M68K_REG_PPC);
	m68k_disassemble(buff, pc, M68K_CPU_TYPE_68000);
	fprintf(stderr, "At %04x: %s\n", pc, buff);

	exit(EXIT_FAILURE);
}


/* Read data from RAM, ROM, or a device */
unsigned int cpu_read_byte(unsigned int address)
{
	if(g_fc & 2)	/* Program */
	{
#if defined(MAX_ROM)
		if(address > MAX_ROM)
			exit_error("Attempted to read byte from ROM address %08x", address);
		return READ_BYTE(g_rom, address);
#endif //MAX_ROM
	}

	/* dbg_port */
	if ((address & 0xfff00) == 0x80100) {
		return debug_port_read(address);
	}
	/* Otherwise it's data space */
	switch(address)
	{
		case UART_CREG_ADDRESS:
			return input_device_status();
		case UART_DREG_ADDRESS:
			return input_device_read();
		default:
			break;
	}
	if(address > MAX_RAM)
		exit_error("Attempted to read byte from RAM address %08x", address);
	return READ_BYTE(g_ram, address);
}

unsigned int cpu_read_word(unsigned int address)
{
	if(g_fc & 2)	/* Program */
	{
#if defined(MAX_ROM)
		if(address > MAX_ROM)
			exit_error("Attempted to read word from ROM address %08x", address);
		return READ_WORD(g_rom, address);
#endif //MAX_ROM
	}

	/* dbg_port */
	if ((address & 0xfff00) == 0x80100) {
		return debug_port_read(address);
	}
	/* Otherwise it's data space */
	switch(address)
	{
		case UART_CREG_ADDRESS:
			return input_device_status();
		case UART_DREG_ADDRESS:
			return input_device_read();
		default:
			break;
	}
	if(address > MAX_RAM)
		exit_error("Attempted to read word from RAM address %08x", address);
	return READ_WORD(g_ram, address);
}

unsigned int cpu_read_long(unsigned int address)
{
	if(g_fc & 2)	/* Program */
	{
#if defined(MAX_ROM)
		if(address > MAX_ROM)
			exit_error("Attempted to read long from ROM address %08x", address);
		return READ_LONG(g_rom, address);
#endif //MAX_ROM
	}

	/* dbg_port */
	if ((address & 0xfff00) == 0x80100) {
		return debug_port_read(address);
	}
	/* Otherwise it's data space */
	switch(address)
	{
		case UART_CREG_ADDRESS:
			return input_device_status();
		case UART_DREG_ADDRESS:
			return input_device_read();
		default:
			break;
	}
	if(address > MAX_RAM)
		exit_error("Attempted to read long from RAM address %08x", address);
	return READ_LONG(g_ram, address);
}


unsigned int cpu_read_word_dasm(unsigned int address)
{
#if defined(MAX_ROM)
	if(address > MAX_ROM)
		exit_error("Disassembler attempted to read word from ROM address %08x", address);
	return READ_WORD(g_rom, address);
#endif //MAX_ROM
	return READ_WORD(g_ram, address);
}

unsigned int cpu_read_long_dasm(unsigned int address)
{
#if defined(MAX_ROM)
	if(address > MAX_ROM)
		exit_error("Dasm attempted to read long from ROM address %08x", address);
	return READ_LONG(g_rom, address);
#endif //MAX_ROM
	return READ_LONG(g_ram, address);
}


/* Write data to RAM or a device */
void cpu_write_byte(unsigned int address, unsigned int value)
{
#if 0
	if(g_fc & 2)	/* Program */
		exit_error("Attempted to write %02x to ROM address %08x", value&0xff, address);
#endif
	/* Otherwise it's data space */
	switch(address)
	{
		case UART_DREG_ADDRESS:
			output_device_write(value);
			return;
		default:
			break;
	}
	if(address > MAX_RAM)
		exit_error("Attempted to write %02x to RAM address %08x", value&0xff, address);
	WRITE_BYTE(g_ram, address, value);
}

void cpu_write_word(unsigned int address, unsigned int value)
{
#if 0
	if(g_fc & 2)	/* Program */
		exit_error("Attempted to write %04x to ROM address %08x", value&0xffff, address);
#endif
	/* Otherwise it's data space */
	switch(address)
	{
		case UART_DREG_ADDRESS:
			output_device_write(value);
			return;
		default:
			break;
	}
	if(address > MAX_RAM)
		exit_error("Attempted to write %04x to RAM address %08x", value&0xffff, address);
	WRITE_WORD(g_ram, address, value);
}

void cpu_write_long(unsigned int address, unsigned int value)
{
#if 0
	if(g_fc & 2)	/* Program */
		exit_error("Attempted to write %08x to ROM address %08x", value, address);
#endif
	/* Otherwise it's data space */
	switch(address)
	{
		case UART_DREG_ADDRESS:
			output_device_write(value);
			return;
		default:
			break;
	}
	if(address > MAX_RAM)
		exit_error("Attempted to write %08x to RAM address %08x", value, address);
	WRITE_LONG(g_ram, address, value);
}

/* Called when the CPU pulses the RESET line */
void cpu_pulse_reset(void)
{
	nmi_device_reset();
	output_device_reset();
	input_device_reset();
}

/* Called when the CPU changes the function code pins */
void cpu_set_fc(unsigned int fc)
{
	g_fc = fc;
}

/* Called when the CPU acknowledges an interrupt */
int cpu_irq_ack(int level)
{
	switch(level)
	{
		case IRQ_NMI_DEVICE:
			return nmi_device_ack();
		case IRQ_INPUT_DEVICE:
			return input_device_ack();
		case IRQ_OUTPUT_DEVICE:
			return output_device_ack();
	}
	return M68K_INT_ACK_SPURIOUS;
}




/* Implementation for the NMI device */
void nmi_device_reset(void)
{
	g_nmi = 0;
}

void nmi_device_update(void)
{
	if(g_nmi)
	{
		g_nmi = 0;
		int_controller_set(IRQ_NMI_DEVICE);
	}
}

int nmi_device_ack(void)
{
	printf("\nNMI\n");fflush(stdout);
	int_controller_clear(IRQ_NMI_DEVICE);
	return M68K_INT_ACK_AUTOVECTOR;
}


/* Implementation for the input device */
void input_device_reset(void)
{
	changemode(1);
	// I have tried (and failed) to flush pending input by doing
	//  while (kbhit())
	//     osd_get_char();
	// but it did not work. (One character remains in input buffer, 
	// by typing second char, it returns 1st char)
	// I don't know why it did not work, but I recognize omitting this code
	// make it works.
	setbuf(stdin, NULL);
	setbuf(stdout, NULL);
	g_input_device_ready = 0;
	int_controller_clear(IRQ_INPUT_DEVICE);
}

void input_device_restore(void)
{
	changemode(0);
}

void input_device_update(void)
{
	if (g_input_device_ready) {
		int_controller_set(IRQ_INPUT_DEVICE);
	}
}

int input_device_ack(void)
{
	return M68K_INT_ACK_AUTOVECTOR;
}

unsigned int input_device_status(void)
{
	unsigned char c = 0;
	if (g_input_device_ready)
		c |= 1;
	if (g_output_device_empty)
		c |= 2;
	return c;
}

unsigned int input_device_read(void)
{
	int value;
	//printf("[");
	value = g_input_device_value;
	// emulate uart_dreg is read.
	int_controller_clear(IRQ_INPUT_DEVICE);
	g_input_device_ready = 0;
	//printf("%02X]", value);
	return value;
}

void input_device_write(unsigned int value)
{
	// do nothing
	(void)value;
}

#if 0
/* Implementation of UART */
int uart_creg_read(void)
{
	unsigned char c = 0;
	if (g_input_device_ready)
		c |= 1;
	if (g_output_device_ready)
		c |= 2;
	return c;
}

int uart_dreg_read(void)
{
	int c = input_device_read();
	//printf("/%02X/", c);
	return c;
}

void uart_dreg_write(unsigned char value)
{
	//printf("~%02X~", value&0xff);
	output_device_write(value);
} 
#endif

//
// get_msec ... with clock_gettime, a new POSIC standard
//
long int get_msec(void)
{
	struct timespec ts;
	static unsigned long int start = 0, current;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	current = (unsigned long int)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; 
	if (start == 0) {
		start = current;
	}
	return current - start;
}

/* Implementation for the output device */
void output_device_reset(void)
{
	g_output_device_last_output = get_msec();
	g_output_device_data_ready = 0;
	g_output_device_empty = 1;
	int_controller_clear(IRQ_OUTPUT_DEVICE);
}

void output_device_update(void)
{
	if(g_output_device_empty)		// empty check if any data is pending
	{
		if (g_output_device_data_ready)	// there is a data to be sent in g_output_device_data
		{
			printf("%c", g_output_device_data);
			g_output_device_data_ready = 0;
			g_output_device_last_output = get_msec();
			g_output_device_empty = 0;
			int_controller_clear(IRQ_OUTPUT_DEVICE);
		}
	} else {	// not empty, now a data is transmitting
		if((get_msec() - g_output_device_last_output) >= OUTPUT_DEVICE_PERIOD)
		{
			g_output_device_empty = 1;
			int_controller_set(IRQ_OUTPUT_DEVICE);
		}
	}
}

int output_device_ack(void)
{
	return M68K_INT_ACK_AUTOVECTOR;
}

unsigned int output_device_read(void)
{
	int_controller_clear(IRQ_OUTPUT_DEVICE);
	return 0;
}

void output_device_write(unsigned int value)
{
	g_output_device_data_ready = 1;
	g_output_device_data = value & 0xff;
	if (g_output_device_empty)
	{
		// send it out to lower physical layer
		// it should be here also, so that short-time consequent output_device_write calling
		// should not overwritten the first output character.
		printf("%c", g_output_device_data);
		g_output_device_data_ready = 0;
		g_output_device_last_output = get_msec();
		g_output_device_empty = 0;
		int_controller_clear(IRQ_OUTPUT_DEVICE);
	}
}

/* debug port implementation */
void monitor(int mode);
static unsigned int g_addr, g_value;
#define GET_ADDR() g_addr

unsigned int debug_port_read(unsigned int addr)
{
	g_addr = addr;
    xprintf("%05lX: (NA) R\n", addr);
    monitor(2);
	return g_value;
}

void debug_port_write(unsigned int addr, unsigned int value)
{
	g_addr = addr;
	g_value = value;
    xprintf("%05lX: %02X W\n", addr, (value&0xff));
    monitor(1);
}

//
// monitor
// monitor_mode: 1 ... DBG_PORT write
//               2 ... DBG_PORT read
//               0 ... other(usually single step mode)
//
void monitor(int mode)
{
	extern int to_hex(char c);
    static char buf[8];
    int c, d;
    
//    xprintf("|%05lX %02X %c ", addr, PORTC, ((RA5) ? 'R' : 'W'));
    
    if (mode == 2) {    // DBG_PORT read
        xprintf(" IN>");
        xgets(buf, 7, 0);
        int i = 0, n = 0;
        while (i < 8 && (c = buf[i++]) && (d = to_hex((unsigned char)c)) >= 0) {
            n *= 16; n += d;
            //xprintf("(%x,%x)", n, d);
        }
		g_value = n;
    } else {
        if (mode == 1) { // DBG_PORT write
            xprintf(" OUT: %02x", g_value);
        }
#if 0
        if ((c = getch()) == '.')
            ss_flag = 0;
        else if (c == 's' || c == ' ')
            ss_flag = 1;
#endif
        xprintf("\n");
    }
}


/* Implementation for the interrupt controller */
void int_controller_set(unsigned int value)
{
	unsigned int old_pending = g_int_controller_pending;

	g_int_controller_pending |= (1<<value);

	if(old_pending != g_int_controller_pending && value > g_int_controller_highest_int)
	{
		g_int_controller_highest_int = value;
		m68k_set_irq(g_int_controller_highest_int);
	}
}

void int_controller_clear(unsigned int value)
{
	g_int_controller_pending &= ~(1<<value);

	for(g_int_controller_highest_int = 7;g_int_controller_highest_int > 0;g_int_controller_highest_int--)
		if(g_int_controller_pending & (1<<g_int_controller_highest_int))
			break;

	m68k_set_irq(g_int_controller_highest_int);
}


/* Parse user input and update any devices that need user input */
void update_user_input(void)
{
	static int last_ch = -1;
	int ch;
	if (g_input_device_ready || !kbhit())
		return;
	while (kbhit()) {
		ch = osd_get_char();
		//printf("=%02X=", ch&0xff);
	}

	switch(ch)
	{
		case 0x1b:
			g_quit = 1;
			break;
		case '~':
			if(last_ch != ch)
				g_nmi = 1;
			break;
		default:
			g_input_device_ready = 1;
			g_input_device_value = ch;
	}
	//printf("(%02X)", ch);
	last_ch = ch;
}

/* Disassembler */
void make_hex(char* buff, unsigned int pc, unsigned int length)
{
	char* ptr = buff;

	for(;length>0;length -= 2)
	{
		sprintf(ptr, "%04x", cpu_read_word_dasm(pc));
		pc += 2;
		ptr += 4;
		if(length > 2)
			*ptr++ = ' ';
	}
}

void disassemble_program()
{
	unsigned int pc;
	unsigned int instr_size;
	char buff[100];
	char buff2[100];

	pc = cpu_read_long_dasm(4);

	while(pc <= 0x16e)
	{
		instr_size = m68k_disassemble(buff, pc, M68K_CPU_TYPE_68000);
		make_hex(buff2, pc, instr_size);
		printf("%03x: %-20s: %s\n", pc, buff2, buff);
		pc += instr_size;
	}
	fflush(stdout);
}

void cpu_instr_callback(int pc)
{
	(void)pc;
/* The following code would print out instructions as they are executed */
/*
	static char buff[100];
	static char buff2[100];
	static unsigned int pc;
	static unsigned int instr_size;

	pc = m68k_get_reg(NULL, M68K_REG_PC);
	instr_size = m68k_disassemble(buff, pc, M68K_CPU_TYPE_68000);
	make_hex(buff2, pc, instr_size);
	printf("E %03x: %-20s: %s\n", pc, buff2, buff);
	fflush(stdout);
*/
}

// bootloader

typedef unsigned long int addr_t;

FILE *xf;

void xprintf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

void xgets(char *buf, int size, int noecho_flag)
{
	noecho_flag = 0;
	changemode(noecho_flag);	// COOKED mode
	fgets(buf, size, stdin);
	changemode(1);	// RAW mode
}

void poke_ram(addr_t addr, unsigned char c)
{
	WRITE_BYTE(g_ram, addr, c);
}

unsigned char peek_ram(addr_t addr)
{
	return READ_BYTE(g_ram, addr);
}

static int uc = -1;
int getchr(void)
{
    static int count = 0;
    int c;
    if (uc >= 0) {
        c = uc;
        uc = -1;
        return c;
    }
    while ((c = fgetc(xf)) == '.' && count++ < 2);
    if (c == '.') {
        count = 0;
        return -1;
    }
    count = 0;
    return c;
}

void ungetchr(int c)
{
    uc = c;
}

int is_hex(char c)
{
    if ('0' <= c && c <= '9')
        return !0;
    c &= ~0x20;     // capitalize
    return ('A' <= c && c <= 'F');
}

int to_hex(char c)
{
    //xprintf("{%c}", c);
    if ('0' <= c && c <= '9')
        return c - '0';
    c &= ~0x20;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void clear_all(void)
{
    addr_t p = 0;
    int i = 0;
    do {
        if ((p & 0xfff) == 0) {
            xprintf("%X", i++);
        }
        poke_ram(p, 0);
    } while (p++ != 0xffff);
}

typedef unsigned short saddr_t;

extern void set_breakpoint_addr(saddr_t a);
extern void set_wordtrace_addr(saddr_t addr);

void manualboot(void)
{
    int c, cc, d, n;
    addr_t addr = 0, max = 0, min = MAX_RAM + 1;
    int addr_flag = 0;
    
    while (1) {
        while ((c = getchr()) == ' ' || c == '\t' || c == '\n' || c == '\r')
            ;   // skip white spaces
        if (c == -1)
            break;
        if (c == '!' && min < max) {
			// dump memory between min and max
            xprintf("\n");
            // dump memory
            addr_t start, end;
            start = min & 0xfff0;
            end = max;
            //if (end > 0x40)
            //    end = 0x40;
            while (start < end) {
                if ((start & 0xf) == 0) {
                    xprintf("%04X ", start);  
                }
                d = ((unsigned short)peek_ram(start))<<8;
                d |= peek_ram(start + 1);
                xprintf("%04X ", d);
                if ((start & 0xf) == 0xe) {
                    xprintf("\n");
                }
                start += 2;                
            }
            continue;
        }
        addr_flag = ((c == '=') || (c == '%') || (c == 'P') || (c == 'Q') || (c == 'R'));
			// P specifies breakpoint address
        cc = c;
        if (!addr_flag)
            ungetchr(c);
        // read one hex value
        n = 0;
        while ((d = to_hex((unsigned char)(c = getchr()))) >= 0) {
            n *= 16; n += d;
            //xprintf("(%x,%x)", n, d);
        }
        if (c < 0)
            break;
        if (d < 0) {
            if (addr_flag) {  // set address
                if (cc == '=')
                    addr = (addr_t)n;
				else if (cc == 'P') {
					// set breakpoint address
					fprintf(stderr, "P%04lX", (addr_t)n);
					set_breakpoint_addr(n);
				} else if (cc == 'Q') {
					// set word trace breakpoint, %a0 has IP
					fprintf(stderr, "Q%04lX", (addr_t)n);
					extern void set_wordtrace_addr(saddr_t a);
					set_breakpoint_addr(n);
					set_wordtrace_addr(n);
				} else if (cc == 'R') {
					// set do_next breakpoint
					// here, %a0 has the address of next-to-jump token
					fprintf(stderr, "R%04lX", (addr_t)n);
					extern void set_donext_addr(saddr_t a);
					set_breakpoint_addr(n);
					set_donext_addr(n);

				}
            } else {
                if (/* 0 <= addr &&*/ addr < (MAX_RAM + 1)) {
                    //xprintf("[%04X] = %02X%02X\n", addr, ((n>>8)&0xff), (n & 0xff));
                    poke_ram(addr++, ((n>>8) & 0xff));
                    poke_ram(addr++, (n & 0xff));
                    if (max < addr)
                        max = addr;
                    if (addr - 2 < min)
                        min = addr - 2;
                }
            }
            continue;
        }
    }
}



/* The main loop */
int main(int argc, char* argv[])
{

	if(argc == 1)
	{
		printf("Usage: sim <program file>...\n");
		exit(-1);
	}

	// boot process
	xprintf(";");		// boot prompt
	for(int i = 1; i < argc; ++i) {
		if((xf = fopen(argv[i], "rb")) == NULL)
			exit_error("Unable to open %s", argv[i]);
		manualboot();
		fclose(xf);
	}
	// read dump format
	printf("\nrun...");fflush(stdout);

#if defined(MAX_ROM)
	if(fread(g_rom, 1, MAX_ROM+1, fhandle) <= 0)
		exit_error("Error reading %s", argv[1]);
#endif //MAX_ROM

//	disassemble_program();

	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_pulse_reset();
	input_device_reset();
	output_device_reset();
	nmi_device_reset();

	g_quit = 0;
	while(!g_quit)
	{
		// Our loop requires some interleaving to allow us to update the
		// input, output, and nmi devices.

		update_user_input();

		// Values to execute determine the interleave rate.
		// Smaller values allow for more accurate interleaving with multiple
		// devices/CPUs but is more processor intensive.
		// 100000 is usually a good value to start at, then work from there.

		// Note that I am not emulating the correct clock speed!
		m68k_execute(1);
		output_device_update();
		input_device_update();
		nmi_device_update();
	}

	input_device_restore();

	return 0;
}

unsigned short peek_word(unsigned short addr)
{
	unsigned short h, l;
	h = peek_ram(addr), l = peek_ram(addr + 1);
	return (h * 256) | l;
}

void dump_linbuf(void)
{
	unsigned char c;
	fprintf(stderr, "linbuf: ");
	for (int i = 0; i < 20; ++i) {
		c = peek_ram(0x3000 + i);
		fprintf(stderr, "%02X ", c);
	}
	fprintf(stderr, "\n");
	fprintf(stderr, "wordbuf: ");
	for (int i = 0; i < 20; ++i) {
		c = peek_ram(0x3080 + i);
		fprintf(stderr, "%02X ", c);
	}
	fprintf(stderr, "\n");
}

void dump_bufchar(const char *name, unsigned short top, int n)
{
	unsigned int a = top;

	for (int i = 0; i < n; ++i) {
		if (i % 16 == 0)
			fprintf(stderr, "%s[%04x]: ", name, a + i);
		unsigned char c = peek_ram(a + i);
		fprintf(stderr, "%02X ", c);
		if (i % 16 == 15)
			fprintf(stderr,"\n");
	}
	fprintf(stderr, "\n");
}

void dump_bufword(const char *name, unsigned short top, int n)
{
	unsigned short a = top, w;
	for (int i = 0; i < n; ++i) {
		if (i % 8 == 0)
			fprintf(stderr, "%s[%04x]: ", name, a + 2 * i);
		w = peek_word(a + 2 * i);
		fprintf(stderr, "%04X ", w);
		if (i % 8 == 7)
			fprintf(stderr,"\n");
	}
	fprintf(stderr, "\n");
}

void dump_tail(void)
{
	dump_bufchar("tail", peek_word(0x2006), 48);
}

void dump_variable(void)
{
	dump_bufword("var", 0x3400, 5);
}

void dump_streambuf(void)
{
	dump_bufchar("streambuf", 0x3100, 32);
}

// word execution trace
addr_t _find(const char *name, int *result_len)
{
	saddr_t np, dp0, dp = peek_word(0x2004);			// last
	saddr_t pp = peek_word(0x2002);				// here
	//fprintf(stderr, "dp = %04X\n", dp);
	while(dp) {
		// match name and entry-string
		dp0 = dp;
		int len = peek_ram(dp) & 0x1f;	// length of entry-name
		//fprintf(stderr, "dp = %04X: len = %d, name = %s, entry = %.*s\n", dp, len, name, len, &g_ram[dp + 1]);
		if ((int)strlen(name) == len && strncmp((const char *)&g_ram[dp+1], name, len) == 0) {
			// got it
			//fprintf(stderr, "found: %04X\n", dp);
			if (result_len)
				*result_len = pp - dp0;
			return dp;
		}
		// not match, seek previous entry
		dp += len + 1;
		if ((dp % 2) == 1) dp++;
		np = peek_word(dp);
		pp = dp0;
		dp = np;
	}
	// not found
	//fprintf(stderr, "not found\n");
	return dp;
}

saddr_t start_trace = 0;
saddr_t end_trace = 0;
saddr_t donext_addr = 0;
saddr_t wordtrace_addr = 0;

void _find_addr(saddr_t addr, saddr_t *startp, saddr_t *endp)
{
	saddr_t np, dp0, dp = peek_word(0x2004);	// last
	saddr_t pp = peek_word(0x2002);				// here
	//fprintf(stderr, "dp = %04X\n", dp);
	while(dp) {
		// match name and entry-string
		dp0 = dp;
		int len = peek_ram(dp) & 0x1f;	// length of entry-name
		//fprintf(stderr, "dp = %04X: len = %d, name = %s, entry = %.*s\n", dp, len, name, len, &g_ram[dp + 1]);
		if (dp0 <= addr && addr < pp) {
			// got it
			if (startp)
				*startp = dp0;
			if (endp)
				*endp = pp;
			//fprintf(stderr, "start: %04X, end: %04X\n", dp0, pp);
			return;
		}
		// not match, seek previous entry
		dp += len + 1;
		if ((dp % 2) == 1) dp++;
		np = peek_word(dp);
		pp = dp0;
		dp = np;
	}
	// not find, do nothing
	//fprintf(stderr, "bad trace addr: %04X\n", addr);
	if (startp)
		*startp = 0;
	if (endp)
		*endp = 0;
	return;
}

void dump_find(void)
{
	char buf[80];
	saddr_t addr;
	int len;
	fprintf(stderr,"name>");
	changemode(0);
	fgets(buf, 79, stdin);
	len = strlen(buf);
	while (len-- > 0 && (buf[len] == '\r' || buf[len] == '\n'))
		buf[len] = '\0';
	changemode(1);
	len = 0;
	addr = _find(buf, &len);
	fprintf(stderr, "result = %04X, len = %d\n", addr, len);
}

void set_wordtrace_addr(saddr_t addr)
{
	fprintf(stderr, "set_wordtrace_addr: %04X\n", addr);
	// %a6 has within target word address
	wordtrace_addr = addr;
}

