#ifndef __WEENET_ATOM_H_
#define __WEENET_ATOM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct weenet_atom {
	const uint32_t len;
	const char str[];
};

struct weenet_atom *weenet_atom_new(const char *str, size_t len);
static size_t weenet_atom_len(struct weenet_atom *a);
static const char *weenet_atom_str(struct weenet_atom *a);

// Atom can be compared equally just 'a == b', this function is provided for
// functions that need an equal-function parameter.
bool weenet_atom_equal(const struct weenet_atom *a, const struct weenet_atom *b);

// The means of return value is same to memcmp().
int weenet_atom_compare(const struct weenet_atom *a, const struct weenet_atom *b);

inline static size_t
weenet_atom_len(struct weenet_atom *a) {
	return (size_t)a->len;
}

inline static const char *
weenet_atom_str(struct weenet_atom *a) {
	return a->str;
}

#endif
