#include "sha1.h"

#include <cstdio>
#include <cstring>

namespace obs_onvif {

namespace {

struct Sha1Context {
	uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
	                 0x10325476u, 0xC3D2E1F0u};
	uint64_t total = 0;
	uint8_t block[64] = {};
	size_t blockUsed = 0;
};

inline uint32_t rol32(uint32_t v, unsigned n)
{
	return (v << n) | (v >> (32u - n));
}

void ProcessBlock(Sha1Context &ctx, const uint8_t *p)
{
	uint32_t w[80];
	for (unsigned i = 0; i < 16; ++i)
		w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
		       (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
	for (unsigned i = 16; i < 80; ++i)
		w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3],
		 e = ctx.h[4];

	for (unsigned i = 0; i < 80; ++i) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999u;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1u;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCu;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6u;
		}
		uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol32(b, 30);
		b = a;
		a = tmp;
	}

	ctx.h[0] += a;
	ctx.h[1] += b;
	ctx.h[2] += c;
	ctx.h[3] += d;
	ctx.h[4] += e;
}

void Update(Sha1Context &ctx, const uint8_t *data, size_t len)
{
	ctx.total += len;
	while (len > 0) {
		size_t take = 64 - ctx.blockUsed;
		if (take > len)
			take = len;
		std::memcpy(ctx.block + ctx.blockUsed, data, take);
		ctx.blockUsed += take;
		data += take;
		len -= take;
		if (ctx.blockUsed == 64) {
			ProcessBlock(ctx, ctx.block);
			ctx.blockUsed = 0;
		}
	}
}

SHA1Digest Final(Sha1Context &ctx)
{
	const uint64_t bitLen = ctx.total * 8;
	uint8_t pad = 0x80;
	Update(ctx, &pad, 1);
	uint8_t zero = 0;
	while (ctx.blockUsed != 56)
		Update(ctx, &zero, 1);

	uint8_t lenField[8];
	for (int i = 0; i < 8; ++i)
		lenField[i] = uint8_t(bitLen >> (8 * (7 - i)));
	Update(ctx, lenField, 8);

	if (ctx.blockUsed != 0) {
		// Should never happen: the padding above always lands at 64 modulo.
		std::memset(ctx.block + ctx.blockUsed, 0, 64 - ctx.blockUsed);
		ProcessBlock(ctx, ctx.block);
	}

	SHA1Digest out;
	for (unsigned i = 0; i < 5; ++i)
		for (unsigned j = 0; j < 4; ++j)
			out[i * 4 + j] = uint8_t(ctx.h[i] >> (8 * (3 - j)));
	return out;
}

} // namespace

SHA1Digest sha1(const void *data, size_t len)
{
	Sha1Context ctx;
	Update(ctx, static_cast<const uint8_t *>(data), len);
	return Final(ctx);
}

SHA1Digest sha1(const std::string &in)
{
	return sha1(in.data(), in.size());
}

namespace {
std::string HexEncode(const SHA1Digest &digest)
{
	static const char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(digest.size() * 2);
	for (uint8_t b : digest) {
		out.push_back(kHex[b >> 4]);
		out.push_back(kHex[b & 0xF]);
	}
	return out;
}
} // namespace

std::string sha1_hex(const void *data, size_t len)
{
	return HexEncode(sha1(data, len));
}

std::string sha1_hex(const std::string &in)
{
	return HexEncode(sha1(in));
}

} // namespace obs_onvif