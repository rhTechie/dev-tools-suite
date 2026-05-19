#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <addr> [width] [count]\n"
            "  addr  : register address, supports decimal or 0x-prefixed hex\n"
            "  width : 8, 16, 32, 64 (default: 32)\n"
            "  count : number of consecutive registers to read (default: 1)\n"
            "\n"
            "Examples:\n"
            "  %s 0xfdc60068\n"
            "  %s 0xfdc60068 32 4\n",
            prog, prog, prog);
}

static int parse_ull(const char *text, unsigned long long *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtoull(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0')) {
        return -1;
    }

    return 0;
}

static int width_to_bytes(unsigned long long width_bits, size_t *width_bytes)
{
    switch (width_bits) {
    case 8:
        *width_bytes = 1;
        return 0;
    case 16:
        *width_bytes = 2;
        return 0;
    case 32:
        *width_bytes = 4;
        return 0;
    case 64:
        *width_bytes = 8;
        return 0;
    default:
        return -1;
    }
}

int main(int argc, char *argv[])
{
    unsigned long long raw_addr = 0;
    unsigned long long raw_width = 32;
    unsigned long long raw_count = 1;
    uintptr_t addr = 0;
    size_t width_bytes = 0;
    size_t index = 0;

    if ((argc < 2) || (argc > 4)) {
        usage(argv[0]);
        return 1;
    }

    if (parse_ull(argv[1], &raw_addr) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[1]);
        return 1;
    }

    if ((argc >= 3) && (parse_ull(argv[2], &raw_width) != 0)) {
        fprintf(stderr, "Invalid width: %s\n", argv[2]);
        return 1;
    }

    if ((argc >= 4) && (parse_ull(argv[3], &raw_count) != 0)) {
        fprintf(stderr, "Invalid count: %s\n", argv[3]);
        return 1;
    }

    if (width_to_bytes(raw_width, &width_bytes) != 0) {
        fprintf(stderr, "Unsupported width: %llu\n", raw_width);
        return 1;
    }

    if (raw_count == 0) {
        fprintf(stderr, "Count must be greater than 0\n");
        return 1;
    }

    if (raw_addr > (unsigned long long)UINTPTR_MAX) {
        fprintf(stderr, "Address 0x%llx does not fit in uintptr_t on this target\n", raw_addr);
        return 1;
    }

    addr = (uintptr_t)raw_addr;
    if ((addr % width_bytes) != 0U) {
        fprintf(stderr,
                "Address 0x%llx is not aligned for %llu-bit access\n",
                raw_addr,
                raw_width);
        return 1;
    }

    for (index = 0; index < (size_t)raw_count; ++index) {
        uintptr_t current = addr + (index * width_bytes);

        switch (raw_width) {
        case 8: {
            volatile const uint8_t *reg = (volatile const uint8_t *)current;
            printf("0x%llx: 0x%02x\n",
                   (unsigned long long)current,
                   (unsigned int)(*reg));
            break;
        }
        case 16: {
            volatile const uint16_t *reg = (volatile const uint16_t *)current;
            printf("0x%llx: 0x%04x\n",
                   (unsigned long long)current,
                   (unsigned int)(*reg));
            break;
        }
        case 32: {
            volatile const uint32_t *reg = (volatile const uint32_t *)current;
            printf("0x%llx: 0x%08" PRIx32 "\n",
                   (unsigned long long)current,
                   *reg);
            break;
        }
        case 64: {
            volatile const uint64_t *reg = (volatile const uint64_t *)current;
            printf("0x%llx: 0x%016" PRIx64 "\n",
                   (unsigned long long)current,
                   *reg);
            break;
        }
        default:
            return 1;
        }
    }

    return 0;
}
