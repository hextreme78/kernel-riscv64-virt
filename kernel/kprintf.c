#include <kernel/kprintf.h>
#include <kernel/uart-ns16550a.h>
#include <kernel/mutex.h>
#include <kernel/irq.h>
#include <kernel/riscv64.h>
#include <stdarg.h>

static mutex_t kprintf_mutex;
uint64_t paniced = 0;

void kprintf_init(void)
{
	mutex_init(&kprintf_mutex);
}

static void __print_decimal(void (*putch)(char), int64_t d)
{
	if (!d) {
		return;
	}
	__print_decimal(putch, d / 10);
	if (d >= 0) {
		putch('0' + d % 10);
	} else {
		putch('0' - d % 10);
	}
}

static void print_decimal(void (*putch)(char), int64_t d)
{
	if (!d) {
		putch('0');
		return;
	} else if (d < 0) {
		putch('-');
	}
	__print_decimal(putch, d);
}

static void __print_unsigned(void (*putch)(char), uint64_t u)
{
	if (!u) {
		return;
	}
	__print_unsigned(putch, u / 10);
	putch('0' + u % 10);
}

static void print_unsigned(void (*putch)(char), uint64_t u)
{
	if (!u) {
		putch('0');
		return;
	}
	__print_unsigned(putch, u);
}

static void __print_hex(void (*putch)(char), uint64_t x)
{
	if (!x) {
		return;
	}
	__print_hex(putch, x / 16);
	if (x % 16 < 10) { 
		putch('0' + x % 16);
	} else {
		putch('a' + x % 16 - 10);
	}
}

static void print_hex(void (*putch)(char), uint64_t x)
{
	if (!x) {
		putch('0');
		return;
	}
	__print_hex(putch, x);
}

static void __print_binary(void (*putch)(char), uint64_t b)
{
	if (!b) {
		return;
	}
	__print_binary(putch, b / 2);
	putch('0' + b % 2);
}

static void print_binary(void (*putch)(char), uint64_t b)
{
	if (!b) {
		putch('0');
		return;
	}
	__print_binary(putch, b);
}

static void print_str(void (*putch)(char), const char *s)
{
	for (size_t i = 0; s[i]; i++) {
		putch(s[i]);
	}
}

static void print_ptr(void (*putch)(char), void *p)
{
	size_t zeroes = sizeof(void *) * 8 / 4;
	uint64_t tmp = (uint64_t) p;
	
	while (tmp) {
		tmp /= 16;
		zeroes--;
	}
	
	for (size_t i = 0; i < zeroes; i++) {
		putch('0');
	}

	__print_hex(putch, (uint64_t) p);
}

static void __kprintf(void (*putch)(char), const char *fmt, va_list args)
{
	for (size_t i = 0; fmt[i]; i++) {
		if (fmt[i] == '%') {
			i++;
			switch (fmt[i]) {
			case '%':
				putch('%');
				break;
			case 'd':
				print_decimal(putch, va_arg(args, int64_t));
				break;
			case 'u':
				print_unsigned(putch, va_arg(args, uint64_t));
				break;	
			case 'x':
				print_hex(putch, va_arg(args, uint64_t));
				break;	
			case 'b':
				print_binary(putch, va_arg(args, uint64_t));
				break;	
			case 's':
				print_str(putch, va_arg(args, const char *));
				break;
			case 'p':
				print_ptr(putch, va_arg(args, void *));
				break;
			default:
				goto badfmt;
			}
		} else {
			putch(fmt[i]);
		}
	}

badfmt:
	;
}

/* kprintf will not work while interrupts are off */
void kprintf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	mutex_lock(&kprintf_mutex);
	__kprintf(uart_putch_async, fmt, args);
	uart_tx_flush_async();
	mutex_unlock(&kprintf_mutex);

	va_end(args);
}

/* use kprintf_s when interrupts are off */
void kprintf_s(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	mutex_lock(&kprintf_mutex);
	uart_tx_flush_sync();
	__kprintf(uart_putch_sync, fmt, args);
	mutex_unlock(&kprintf_mutex);

	va_end(args);
}

void panic(const char *msg)
{
	irq_off();
	paniced = 1;
	kprintf_s("panic: %s\n", msg);
	while (1);
}
