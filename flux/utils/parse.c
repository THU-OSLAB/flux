#include <limits.h>

#include <utils/parse.h>

static bool flux_ascii_digit(char ch, unsigned int *digit)
{
	if (ch >= '0' && ch <= '9')
		*digit = (unsigned int)(ch - '0');
	else if (ch >= 'a' && ch <= 'f')
		*digit = (unsigned int)(ch - 'a') + 10;
	else if (ch >= 'A' && ch <= 'F')
		*digit = (unsigned int)(ch - 'A') + 10;
	else
		return false;
	return true;
}

static bool flux_parse_magnitude(const char *first, const char *last,
				 unsigned int base, uint64_t limit,
				 uint64_t *value)
{
	uint64_t result = 0;
	const char *cursor;

	if (!first || !last || !value || first >= last ||
	    (base != 0 && base != 10 && base != 16))
		return false;
	if (base == 0) {
		base = 10;
		if (last - first >= 2 && first[0] == '0' &&
		    (first[1] == 'x' || first[1] == 'X')) {
			base = 16;
			first += 2;
		}
	} else if (base == 16 && last - first >= 2 && first[0] == '0' &&
		   (first[1] == 'x' || first[1] == 'X')) {
		first += 2;
	}
	if (first == last)
		return false;

	for (cursor = first; cursor < last; cursor++) {
		unsigned int digit;

		if (!flux_ascii_digit(*cursor, &digit) || digit >= base ||
		    result > (limit - digit) / base)
			return false;
		result = result * base + digit;
	}
	*value = result;
	return true;
}

bool flux_parse_u64_ascii(const char *first, const char *last,
			  unsigned int base, uint64_t *value)
{
	return flux_parse_magnitude(first, last, base, UINT64_MAX, value);
}

bool flux_parse_i64_ascii(const char *first, const char *last,
			  unsigned int base, int64_t *value)
{
	uint64_t magnitude;
	uint64_t limit = INT64_MAX;
	bool negative = false;

	if (!first || !last || !value || first >= last)
		return false;
	if (*first == '-' || *first == '+') {
		negative = *first == '-';
		first++;
		if (first == last)
			return false;
	}
	if (negative)
		limit++;
	if (!flux_parse_magnitude(first, last, base, limit, &magnitude))
		return false;
	if (negative && magnitude == (uint64_t)INT64_MAX + 1)
		*value = INT64_MIN;
	else
		*value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
	return true;
}
