// stb_deflate.h: the zlib/deflate COMPRESSOR of libstb's stb_image_write.h, extracted.
//
// Origin: https://github.com/MohammadRaziei/libstb, src/third_party/stb/stb_image_write.h (which is Sean
// Barrett's stb_image_write.h v1.16 with libstb's rewrite of stbi_zlib_compress: 4-byte hash-chain LZ77 with lazy
// matching and per-block choice of dynamic / fixed / stored Huffman blocks). stb: https://github.com/nothings/stb.
// License: MIT or public domain, your choice (the notice is at the end of this file and in LICENSE).
//
// The text between the two "BEGIN/END verbatim" markers is byte for byte stb_image_write.h lines 828 to 1284 of that
// libstb file. Everything outside the markers is the small shim that lets it compile on its own. The output of
// stbi_zlib_compress is a plain zlib stream (RFC 1950). Include it from ONE .cpp only: every function is static.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#define STBIWDEF static
#define STBIW_MALLOC(sz) std::malloc(sz)
#define STBIW_FREE(p) std::free(p)
#define STBIW_UCHAR(x) (unsigned char)((x) & 0xff)

// ---- BEGIN verbatim: libstb stb_image_write.h, lines 828-1284 ----
#ifndef STBIW_ZLIB_COMPRESS

// ---------------------------------------------------------------------------
// Built-in zlib compressor.
//
//   * LZ77 with a 3-byte hash chain (fixed arrays, no per-bucket allocation)
//     and zlib-style lazy matching; `quality` sets how hard it searches.
//   * Dynamic Huffman blocks: every block gets its own length-limited
//     canonical code, and the cheapest of dynamic / fixed / stored is written.
//
// The output is a plain zlib stream that any inflater reads.

typedef unsigned long long stbiw__zu64;

#define stbiw__ZWINDOW    32768
#define stbiw__ZHBITS     16
#define stbiw__ZHSIZE     (1 << stbiw__ZHBITS)
#define stbiw__ZBLOCKTOK  65536   // tokens per deflate block; the Huffman codes are rebuilt for each block

static const unsigned short stbiw__zlenbase[29]   = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const unsigned char  stbiw__zlenextra[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const unsigned short stbiw__zdistbase[30]  = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const unsigned char  stbiw__zdistextra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static const unsigned char  stbiw__zclorder[19]   = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

typedef struct
{
   unsigned char *out;
   int n;                 // bytes written; capacity is reserved up front (see stbi_zlib_compress)
   stbiw__zu64 bits;
   int nbits;

   const unsigned char *data;
   unsigned int *tok;     // literal: the byte (< 256); match: (length << 16) | distance
   int ntok;
   int raw_start, raw_len;       // input bytes covered by the tokens of the current block
   int extra_bits;               // length / distance extra bits of the current block
   unsigned int lfreq[288], dfreq[32];

   unsigned short len_sym[259];  // match length -> literal/length symbol (257..285)
   unsigned char dtab[512];      // distance -> distance symbol (see stbiw__zdsym)
   unsigned char  fl[288], fdl[30];   // fixed Huffman code lengths
   unsigned short fc[288], fdc[30];   // fixed Huffman codes (bit-reversed for output)
} stbiw__zctx;

#define stbiw__zput(z, v, k) do { \
      (z)->bits |= (stbiw__zu64)(v) << (z)->nbits; (z)->nbits += (k); \
      if ((z)->nbits >= 32) { \
         stbiw__zu64 b_ = (z)->bits; unsigned char *o_ = (z)->out + (z)->n; \
         o_[0] = (unsigned char) b_; o_[1] = (unsigned char) (b_ >> 8); \
         o_[2] = (unsigned char) (b_ >> 16); o_[3] = (unsigned char) (b_ >> 24); \
         (z)->n += 4; (z)->bits >>= 32; (z)->nbits -= 32; } \
   } while (0)

static void stbiw__zalign(stbiw__zctx *z)
{
   while (z->nbits > 0) {
      z->out[z->n++] = (unsigned char) z->bits;
      z->bits >>= 8;
      z->nbits -= 8;
   }
   z->bits = 0;
   z->nbits = 0;
}

static int stbiw__zlib_bitrev(int code, int codebits)
{
   int res=0;
   while (codebits--) {
      res = (res << 1) | (code & 1);
      code >>= 1;
   }
   return res;
}

static int stbiw__zcmp_u32(const void *a, const void *b)
{
   unsigned int x = *(const unsigned int *) a, y = *(const unsigned int *) b;
   return x < y ? -1 : x > y ? 1 : 0;
}

// Code lengths (all <= maxlen) for freq[0..n-1]. At least two symbols must have a
// non-zero frequency, and n <= 288. Huffman by the two-queue method; if the tree
// is too deep the frequencies are flattened and it is rebuilt (slightly
// suboptimal, always valid).
static void stbiw__zhuff_lengths(const unsigned int *freq, int n, int maxlen, unsigned char *len)
{
   unsigned int work[288], key[288], w[600];
   int parent[600], depth[600];
   int i, k, m, li, ni, nn, maxd;
   for (i=0; i < n; ++i) work[i] = freq[i];
   for (;;) {
      m = 0;
      for (i=0; i < n; ++i) if (work[i]) key[m++] = (work[i] << 9) | (unsigned int) i;
      qsort(key, (size_t) m, sizeof(key[0]), stbiw__zcmp_u32);
      for (i=0; i < m; ++i) w[i] = key[i] >> 9;
      li = 0; ni = m; nn = m;
      for (k=0; k < m-1; ++k) {
         int a, b;
         if (li < m && (ni >= nn || w[li] <= w[ni])) a = li++; else a = ni++;
         if (li < m && (ni >= nn || w[li] <= w[ni])) b = li++; else b = ni++;
         w[nn] = w[a] + w[b];
         parent[a] = parent[b] = nn;
         ++nn;
      }
      depth[nn-1] = 0;
      maxd = 0;
      for (i=nn-2; i >= 0; --i) {
         depth[i] = depth[parent[i]] + 1;
         if (i < m && depth[i] > maxd) maxd = depth[i];
      }
      if (maxd <= maxlen) break;
      for (i=0; i < n; ++i) if (work[i]) work[i] = (work[i] + 1) >> 1;
   }
   for (i=0; i < n; ++i) len[i] = 0;
   for (i=0; i < m; ++i) len[key[i] & 511] = (unsigned char) depth[i];
}

// Canonical codes for the given lengths, bit-reversed so they can be written LSB first.
static void stbiw__zhuff_codes(const unsigned char *len, int n, unsigned short *code)
{
   int count[16], next[16], i, c = 0;
   for (i=0; i < 16; ++i) count[i] = 0;
   for (i=0; i < n; ++i) count[len[i]]++;
   count[0] = 0;
   next[0] = 0;
   for (i=1; i < 16; ++i) { c = (c + count[i-1]) << 1; next[i] = c; }
   for (i=0; i < n; ++i)
      code[i] = len[i] ? (unsigned short) stbiw__zlib_bitrev(next[len[i]]++, len[i]) : 0;
}

#define stbiw__zdsym(c, d) ((d) <= 256 ? (c)->dtab[(d)-1] : (c)->dtab[256 + (((d)-1) >> 7)])

static void stbiw__zwrite_tokens(stbiw__zctx *c, const unsigned short *lc, const unsigned char *ll,
                                 const unsigned short *dc, const unsigned char *dl)
{
   int i;
   for (i=0; i < c->ntok; ++i) {
      unsigned int t = c->tok[i];
      if (t < 256) {
         stbiw__zput(c, lc[t], ll[t]);
      } else {
         int len = (int) (t >> 16), dist = (int) (t & 0xffff);
         int ls = c->len_sym[len], ds = stbiw__zdsym(c, dist);
         stbiw__zput(c, lc[ls], ll[ls]);
         if (stbiw__zlenextra[ls-257]) stbiw__zput(c, len - stbiw__zlenbase[ls-257], stbiw__zlenextra[ls-257]);
         stbiw__zput(c, dc[ds], dl[ds]);
         if (stbiw__zdistextra[ds]) stbiw__zput(c, dist - stbiw__zdistbase[ds], stbiw__zdistextra[ds]);
      }
   }
   stbiw__zput(c, lc[256], ll[256]);
}

// Writes the tokens collected so far as one block (the cheapest of dynamic, fixed, stored).
static void stbiw__zflush(stbiw__zctx *c, int final)
{
   unsigned int lf[288], df[32], clf[19];
   unsigned char ll[288], dl[32], cl[19], rle[320], rlx[320], seq[320];
   unsigned short lc[288], dc[32], cc[19];
   int i, used, nlit, ndist, ncl, nrle, total, run, v, r, nsub;
   unsigned int dyn, fix, sto;

   c->lfreq[256] = 1;   // end of block

   for (i=0; i < 286; ++i) lf[i] = c->lfreq[i];
   for (i=0; i < 30; ++i) df[i] = c->dfreq[i];
   for (used=0, i=0; i < 286; ++i) used += lf[i] != 0;
   for (i=0; i < 286 && used < 2; ++i) if (!lf[i]) { lf[i] = 1; ++used; }
   for (used=0, i=0; i < 30; ++i) used += df[i] != 0;
   for (i=0; i < 30 && used < 2; ++i) if (!df[i]) { df[i] = 1; ++used; }
   stbiw__zhuff_lengths(lf, 286, 15, ll);
   stbiw__zhuff_lengths(df, 30, 15, dl);
   stbiw__zhuff_codes(ll, 286, lc);
   stbiw__zhuff_codes(dl, 30, dc);

   nlit = 286; while (nlit > 257 && ll[nlit-1] == 0) --nlit;
   ndist = 30; while (ndist > 1 && dl[ndist-1] == 0) --ndist;

   // run-length code the code lengths (16: repeat previous, 17/18: runs of zeros)
   total = nlit + ndist;
   for (i=0; i < nlit; ++i) seq[i] = ll[i];
   for (i=0; i < ndist; ++i) seq[nlit+i] = dl[i];
   nrle = 0;
   for (i=0; i < total; ) {
      v = seq[i];
      run = 1;
      while (i+run < total && seq[i+run] == v) ++run;
      if (v == 0 && run >= 3) {
         i += run;
         while (run > 0) {
            if (run >= 11) { r = run > 138 ? 138 : run; rle[nrle] = 18; rlx[nrle++] = (unsigned char) (r-11); run -= r; }
            else if (run >= 3) { rle[nrle] = 17; rlx[nrle++] = (unsigned char) (run-3); run = 0; }
            else { while (run--) { rle[nrle] = 0; rlx[nrle++] = 0; } run = 0; }
         }
      } else {
         rle[nrle] = (unsigned char) v; rlx[nrle++] = 0;
         ++i; --run;
         while (run >= 3) { r = run > 6 ? 6 : run; rle[nrle] = 16; rlx[nrle++] = (unsigned char) (r-3); run -= r; i += r; }
         while (run > 0) { rle[nrle] = (unsigned char) v; rlx[nrle++] = 0; --run; ++i; }
      }
   }
   for (i=0; i < 19; ++i) clf[i] = 0;
   for (i=0; i < nrle; ++i) clf[rle[i]]++;
   for (used=0, i=0; i < 19; ++i) used += clf[i] != 0;
   for (i=0; i < 19 && used < 2; ++i) if (!clf[i]) { clf[i] = 1; ++used; }
   stbiw__zhuff_lengths(clf, 19, 7, cl);
   stbiw__zhuff_codes(cl, 19, cc);
   ncl = 19; while (ncl > 4 && cl[stbiw__zclorder[ncl-1]] == 0) --ncl;

   // what would each kind of block cost, in bits?
   dyn = 3 + 14 + 3 * (unsigned int) ncl + (unsigned int) c->extra_bits;
   for (i=0; i < nrle; ++i) dyn += cl[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
   for (i=0; i < 286; ++i) dyn += c->lfreq[i] * ll[i];
   for (i=0; i < 30; ++i) dyn += c->dfreq[i] * dl[i];
   fix = 3 + (unsigned int) c->extra_bits;
   for (i=0; i < 286; ++i) fix += c->lfreq[i] * c->fl[i];
   for (i=0; i < 30; ++i) fix += c->dfreq[i] * 5;
   nsub = c->raw_len ? (c->raw_len + 65534) / 65535 : 1;
   sto = (unsigned int) c->raw_len * 8 + (unsigned int) nsub * 48;

   if (sto <= dyn && sto <= fix) {
      const unsigned char *p = c->data + c->raw_start;
      int left = c->raw_len;
      for (i=0; i < nsub; ++i) {
         int blk = left > 65535 ? 65535 : left;
         stbiw__zput(c, final && i == nsub-1, 1);
         stbiw__zput(c, 0, 2);
         stbiw__zalign(c);
         c->out[c->n++] = (unsigned char) blk;        c->out[c->n++] = (unsigned char) (blk >> 8);
         c->out[c->n++] = (unsigned char) ~blk;       c->out[c->n++] = (unsigned char) (~blk >> 8);
         memcpy(c->out + c->n, p, (size_t) blk);
         c->n += blk; p += blk; left -= blk;
      }
   } else if (fix <= dyn) {
      stbiw__zput(c, final, 1);
      stbiw__zput(c, 1, 2);
      stbiw__zwrite_tokens(c, c->fc, c->fl, c->fdc, c->fdl);
   } else {
      stbiw__zput(c, final, 1);
      stbiw__zput(c, 2, 2);
      stbiw__zput(c, nlit - 257, 5);
      stbiw__zput(c, ndist - 1, 5);
      stbiw__zput(c, ncl - 4, 4);
      for (i=0; i < ncl; ++i) stbiw__zput(c, cl[stbiw__zclorder[i]], 3);
      for (i=0; i < nrle; ++i) {
         stbiw__zput(c, cc[rle[i]], cl[rle[i]]);
         if (rle[i] == 16) stbiw__zput(c, rlx[i], 2);
         else if (rle[i] == 17) stbiw__zput(c, rlx[i], 3);
         else if (rle[i] == 18) stbiw__zput(c, rlx[i], 7);
      }
      stbiw__zwrite_tokens(c, lc, ll, dc, dl);
   }

   c->raw_start += c->raw_len;
   c->raw_len = 0;
   c->ntok = 0;
   c->extra_bits = 0;
   memset(c->lfreq, 0, sizeof(c->lfreq));
   memset(c->dfreq, 0, sizeof(c->dfreq));
}

static void stbiw__zadd_lit(stbiw__zctx *c, int b)
{
   c->tok[c->ntok++] = (unsigned int) b;
   c->lfreq[b]++;
   c->raw_len++;
   if (c->ntok >= stbiw__ZBLOCKTOK) stbiw__zflush(c, 0);
}

static void stbiw__zadd_match(stbiw__zctx *c, int len, int dist)
{
   int ls = c->len_sym[len], ds = stbiw__zdsym(c, dist);
   c->tok[c->ntok++] = ((unsigned int) len << 16) | (unsigned int) dist;
   c->lfreq[ls]++;
   c->dfreq[ds]++;
   c->extra_bits += stbiw__zlenextra[ls-257] + stbiw__zdistextra[ds];
   c->raw_len += len;
   if (c->ntok >= stbiw__ZBLOCKTOK) stbiw__zflush(c, 0);
}

static unsigned int stbiw__zload32(const unsigned char *p)
{
   unsigned int v;
   memcpy(&v, p, 4);
   return v;
}

static int stbiw__zmatchlen(const unsigned char *a, const unsigned char *b, int maxlen)
{
   int l = 0;
   while (l + 8 <= maxlen) {
      stbiw__zu64 x, y;
      memcpy(&x, a+l, 8); memcpy(&y, b+l, 8);
      if (x != y) break;
      l += 8;
   }
   while (l < maxlen && a[l] == b[l]) ++l;
   return l;
}

#define stbiw__zhash4(p) (stbiw__zload32(p) * 0x9E3779B1u >> (32 - stbiw__ZHBITS))

// Longest match for d[pos..] that is longer than `best`; *dist is set when one is found.
static int stbiw__zfind(const unsigned char *d, int pos, int n, const int *head, const int *prev,
                        int best, int chain, int nice, int *dist)
{
   int maxlen = n - pos, cand;
   const unsigned char *b = d + pos;
   if (maxlen > 258) maxlen = 258;
   if (nice > maxlen) nice = maxlen;
   if (best >= maxlen) return best;
   cand = head[stbiw__zhash4(b)];
   while (cand >= 0 && pos - cand <= stbiw__ZWINDOW && chain-- > 0) {
      const unsigned char *a = d + cand;
      if (a[best] == b[best] && stbiw__zload32(a) == stbiw__zload32(b)) {
         int l = stbiw__zmatchlen(a, b, maxlen);
         if (l > best) {
            best = l;
            *dist = pos - cand;
            if (l >= nice) break;
         }
      }
      {
         int next = prev[cand & (stbiw__ZWINDOW-1)];
         if (next >= cand) break;
         cand = next;
      }
   }
   return best;
}

#endif // STBIW_ZLIB_COMPRESS

STBIWDEF unsigned char * stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality)
{
#ifdef STBIW_ZLIB_COMPRESS
   // user provided a zlib compress implementation, use that
   return STBIW_ZLIB_COMPRESS(data, data_len, out_len, quality);
#else // use builtin
   // search effort by quality: { good length, max lazy length (0 = greedy), nice length, max chain }
   static const unsigned short effort[11][4] = {
      {4,0,8,4}, {4,0,8,4}, {4,0,16,8}, {4,0,32,32}, {4,4,16,16}, {8,16,32,32},
      {8,16,64,64}, {8,16,128,128}, {8,32,128,256}, {32,128,258,1024}, {32,258,258,4096}
   };
   stbiw__zctx *c;
   int *head, *prev;
   int i, j, pos, cap, good, lazy, nice, maxchain;
   int pend_len = 0, pend_dist = 0, pend_lit = 0;

   if (quality < 0) quality = 0;
   if (quality > 10) quality = 10;
   good = effort[quality][0]; lazy = effort[quality][1]; nice = effort[quality][2]; maxchain = effort[quality][3];

   c = (stbiw__zctx *) STBIW_MALLOC(sizeof(*c));
   head = (int *) STBIW_MALLOC(2 * stbiw__ZHSIZE * sizeof(int));
   // worst case: every block ends up stored (5 bytes of header per 64K) plus the zlib wrapper
   cap = data_len + data_len / 4096 + 1024;
   if (!c || !head || !(c->tok = (unsigned int *) STBIW_MALLOC(stbiw__ZBLOCKTOK * sizeof(unsigned int)))
       || !(c->out = (unsigned char *) STBIW_MALLOC((size_t) cap))) {
      if (c) { STBIW_FREE(c->tok); STBIW_FREE(c); }
      STBIW_FREE(head);
      return NULL;
   }
   prev = head + stbiw__ZHSIZE;
   memset(head, 0xff, stbiw__ZHSIZE * sizeof(int));   // -1: empty

   c->data = data;
   c->n = 0; c->bits = 0; c->nbits = 0;
   c->ntok = 0; c->raw_start = 0; c->raw_len = 0; c->extra_bits = 0;
   memset(c->lfreq, 0, sizeof(c->lfreq));
   memset(c->dfreq, 0, sizeof(c->dfreq));

   // lookup tables: length -> symbol, distance -> symbol, fixed Huffman code
   for (i=0; i < 3; ++i) c->len_sym[i] = 257;   // never used: lengths start at 3
   for (j=0; j < 29; ++j) {
      int hi = j < 28 ? stbiw__zlenbase[j+1] - 1 : 258;
      for (i=stbiw__zlenbase[j]; i <= hi; ++i) c->len_sym[i] = (unsigned short) (257 + j);
   }
   for (j=0; j < 30; ++j) {
      int lo = stbiw__zdistbase[j] - 1, hi = lo + (1 << stbiw__zdistextra[j]) - 1;
      if (hi < 256) for (i=lo; i <= hi; ++i) c->dtab[i] = (unsigned char) j;
      else for (i=lo >> 7; i <= hi >> 7; ++i) c->dtab[256+i] = (unsigned char) j;
      c->fdl[j] = 5;
      c->fdc[j] = (unsigned short) stbiw__zlib_bitrev(j, 5);
   }
   for (i=0; i < 288; ++i) c->fl[i] = (unsigned char) (i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
   stbiw__zhuff_codes(c->fl, 288, c->fc);

   c->out[c->n++] = 0x78;   // zlib header: deflate, 32K window
   c->out[c->n++] = 0x5e;

   pos = 0;
   while (pos < data_len) {
      int len = 0, dist = 0, have = data_len - pos >= 4;
      if (have && pend_len < lazy + (lazy == 0)) {
         int floor = pend_len > 3 ? pend_len : 3;
         len = stbiw__zfind(data, pos, data_len, head, prev, floor, pend_len >= good ? maxchain >> 2 : maxchain, nice, &dist);
         if (len <= floor) len = 0;
      }
      if (lazy == 0) {
         // greedy
         if (len >= 4) {
            stbiw__zadd_match(c, len, dist);
            for (i=0; i < len; ++i, ++pos)
               if (data_len - pos >= 4) { int h = (int) stbiw__zhash4(data+pos); prev[pos & (stbiw__ZWINDOW-1)] = head[h]; head[h] = pos; }
         } else {
            stbiw__zadd_lit(c, data[pos]);
            if (have) { int h = (int) stbiw__zhash4(data+pos); prev[pos & (stbiw__ZWINDOW-1)] = head[h]; head[h] = pos; }
            ++pos;
         }
         continue;
      }
      if (have) { int h = (int) stbiw__zhash4(data+pos); prev[pos & (stbiw__ZWINDOW-1)] = head[h]; head[h] = pos; }
      if (pend_len >= 4 && len <= pend_len) {
         // the match found one byte ago wins
         int end = pos - 1 + pend_len;
         stbiw__zadd_match(c, pend_len, pend_dist);
         for (i=pos+1; i < end; ++i)
            if (data_len - i >= 4) { int h = (int) stbiw__zhash4(data+i); prev[i & (stbiw__ZWINDOW-1)] = head[h]; head[h] = i; }
         pos = end;
         pend_len = 0; pend_lit = 0;
      } else {
         if (pend_lit) stbiw__zadd_lit(c, data[pos-1]);
         pend_len = len; pend_dist = dist; pend_lit = 1;
         ++pos;
      }
   }
   if (pend_lit) stbiw__zadd_lit(c, data[pos-1]);
   stbiw__zflush(c, 1);
   stbiw__zalign(c);

   {
      // adler32 of the input, big endian
      unsigned int s1=1, s2=0;
      int blocklen = (int) (data_len % 5552);
      j=0;
      while (j < data_len) {
         for (i=0; i < blocklen; ++i) { s1 += data[j+i]; s2 += s1; }
         s1 %= 65521; s2 %= 65521;
         j += blocklen;
         blocklen = 5552;
      }
      c->out[c->n++] = STBIW_UCHAR(s2 >> 8);
      c->out[c->n++] = STBIW_UCHAR(s2);
      c->out[c->n++] = STBIW_UCHAR(s1 >> 8);
      c->out[c->n++] = STBIW_UCHAR(s1);
   }
   {
      unsigned char *result = c->out;
      *out_len = c->n;
      STBIW_FREE(c->tok);
      STBIW_FREE(c);
      STBIW_FREE(head);
      return result;
   }
#endif // STBIW_ZLIB_COMPRESS
}
// ---- END verbatim ----

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* The stb license, copied from the end of stb_image_write.h:

------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/
