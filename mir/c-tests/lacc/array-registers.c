static const char *x86_64_registers[][4] = {
	{ "al",   "ax",   "eax",  "rax" },
	{ "bl",   "bx",   "ebx",  "rbx" },
	{ "cl",   "cx",   "ecx",  "rcx" },
	{ "dl",   "dx",   "edx",  "rdx" },
	{ "bpl",  "bp",   "ebp",  "rbp" },
	{ "spl",  "sp",   "esp",  "rsp" },
	{ "sil",  "si",   "esi",  "rsi" },
	{ "dil",  "di",   "edi",  "rdi" },
	{ "r8b",  "r8w",  "r8d",  "r8"  },
	{ "r9b",  "r9w",  "r9d",  "r9"  },
	{ "r10b", "r10w", "r10d", "r10" },
	{ "r11b", "r11w", "r11d", "r11" },
	{ "r12b", "r12w", "r12d", "r12" },
	{ "r13b", "r13w", "r13d", "r13" },
	{ "r14b", "r14w", "r14d", "r14" },
	{ "r15b", "r15w", "r15d", "r15" },
};

#define REG(r, w) \
	x86_64_registers[(int) (r)][(w) == 8 ? 3 : (w) == 4 ? 2 : (w) - 1]

int puts(const char *s);

int main(void) {
	int r = 0, w = 4;

	puts(REG(r, w));
	return r + w;
}
