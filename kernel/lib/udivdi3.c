typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;

static uint64_t __udivmoddi3(uint64_t num, uint64_t den, int mod)
{
    uint64_t quot = 0, qbit = 1;

    if (den == 0) return 0;
    if (den == num) return mod ? 0 : 1;

    while ((int64_t)den > 0 && den <= num) {
        den <<= 1;
        qbit <<= 1;
    }
    while (qbit) {
        if (num >= den) {
            num -= den;
            quot += qbit;
        }
        den >>= 1;
        qbit >>= 1;
    }
    return mod ? num : quot;
}

uint64_t __udivdi3(uint64_t num, uint64_t den)
{
    return __udivmoddi3(num, den, 0);
}

uint64_t __umoddi3(uint64_t num, uint64_t den)
{
    return __udivmoddi3(num, den, 1);
}

int64_t __divdi3(int64_t num, int64_t den)
{
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    if (den < 0) { den = -den; neg ^= 1; }
    return neg ? -(int64_t)__udivdi3((uint64_t)num, (uint64_t)den) : (int64_t)__udivdi3((uint64_t)num, (uint64_t)den);
}

int64_t __moddi3(int64_t num, int64_t den)
{
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    if (den < 0) { den = -den; }
    return neg ? -(int64_t)__umoddi3((uint64_t)num, (uint64_t)den) : (int64_t)__umoddi3((uint64_t)num, (uint64_t)den);
}

uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem)
{
    uint64_t quot = __udivmoddi3(num, den, 0);
    if (rem) *rem = __udivmoddi3(num, den, 1);
    return quot;
}

/* signed quotient and remainder in one call: newer GCC emits it for code that uses both */
int64_t __divmoddi4(int64_t num, int64_t den, int64_t *rem)
{
    int64_t q = __divdi3(num, den);
    if (rem) *rem = num - q * den;
    return q;
}
