/* Copyright (c) 2019, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef NO_EMBEDDED_ACCESS_CHECKS

#define magic_bits 30
/*
  Returns a number which, if sorted in descending order, magically puts
  patterns in the order from most specific (e.g. no wildcards) to most generic
  (e.g. "%"). That is, the larger the number, the more specific the pattern is.

  Takes a template that lists types of following patterns (by the first letter
  of _h_ostname, _d_bname, _u_sername) and up to four patterns.
  No more than two can be of 'h' or 'd' type (because one magic value takes
  magic_bits bits, see below).

  ========================================================================

  Here's how the magic is created:

  Let's look at one iteration of the for() loop. That's one pattern.  With
  wildcards (usernames aren't interesting).

  By definition a pattern A is "more specific" than pattern B if the set of
  strings that match the pattern A is smaller than the set of strings that
  match the pattern B. Strings are taken from the big superset of all valid
  utf8 strings up to the maxlen.

  Strings are matched character by character. For every non-wildcard
  character there can be only one matching character in the matched string.

  For a wild_one character ('_') any valid utf8 character will do. Below
  numchars would mean a total number of vaid utf8 characters. It's a huge
  number. A number of matching strings for wild_one will be numchars.

  For a wild_many character ('%') any number of valid utf8 characters will do.
  How many string will match it depends on the amount of non-wild_many
  characters.  Say, if a number of non-wildcard characters is N, and a number
  of wild_one characters is M, and the number of wild_many characters is K,
  then for K=1 its wild_many character will match any number of valid utf8
  characters from 0 to L=maxlen-N-M. The number of matching strings will be

     1 + numchars + numchars^2 + numchars^3 + ... + numchars^L

  Intermediate result: if M=K=0, the pattern will match only one string,
  if M>0, K=0, the pattern will match numchars^M strings, if K=1, the
  pattern will match

     numchars^M + 1 + numchars + numchars^2 + ... + numchars^L

  For a more visual notation, let's write these huge numbers not as
  decimal or binary, but base numchars. Then the last number will be
  a sum of two numbers: the first is one followed by M zeros, the second
  constists of L+1 ones:

    1000{...M...}000 + 111{...L+1...}1111

  This could produce any of the following

    111...112111...1111       if L > M, K = 1
    100...001111...1111       if M > L, K = 1
    2111111...111111111       if M = L, K = 1
    1111111...111111111       if M = 0, K = 1
    1000000...000000000       if K = 0, M > 0

  There are two complications caused by multiple wild_many characters.
  For, say, two wild_many characters, either can accept any number of utf8
  characters, as long the the total amount of them is less then or equal to L.
  Same logic applies to any number of non-consequent wild_many characters
  (consequent wild_many characters count as one). This gives the number of
  matching strings of

    1 + F(K,1)*numchars + F(K,2)*numchars^2 + ... + F(K,L)*numchars^L

  where F(K,R) is the "number of ways one can put R balls into K boxes",
  that is C^{K-1}_{R+K-1}.

  In the "base numchars" notation, it means that besides 0, 1, and 2,
  an R-th digit can be F(K,R). For the purpose of comparison, we only need
  to know the most significant digit, F(K, L).
  While it can be huge, we don't need the exact value, it's a
  a monotonously increasing function of K, so if K1>K2, F(K1,L) > F(K2,L)
  and we can simply compare values of K instead of complex F(K,L).

  The second complication: F(K,R) gives only an upper boundary, the
  actual number of matched strings can be smaller.
  Example: pattern "a%b%c" can match "abbc" as a(b)b()c, and as a()b(b)c.
  F(2,1) = 2, but it's only one string "abbc".
  We'll ignore it here under assumption that it almost never happens
  in practice and this simplification won't noticeably disrupt the ordering.

  The last detail: old get_sort function sorted by the non-wildcard prefix
  length, so in "abc_" and "a_bc" the former one was sorted first. Strictly
  speaking they're both equally specific, but to preserve the backward
  compatible sorting we'll use the P "prefix length or 0 if no wildcards"
  to break ties.

  Now, let's compare two long numbers. Numbers are easy to compare,
  the longer number is larger. If they both have the same lengths,
  the one with the larger first digit is larger, and so on.

  But there is no need to actually calculate these numbers.
  Three numbers L, K, M (and P to break ties) are enough to describe a pattern
  for a purpose of comparison. L/K/M triplets can be compared like this:

  * case 1: if for both patterns L>M: compare L, K, M, in that order
    because:
      - if L1 > L2, the first number is longer
      - If L1 == L2, then the first digit is a monotonously increasing function
        of K, so the first digit is larger when K is larger
      - if K1 == K2, then all other digits in these numbers would be the
        same too, with the exception of one digit in the middle that
        got +1 because of +1000{...M...}000. So, whatever number has a
        larger M will get this +1 first.
  * case 2: if for both patterns L<M: compare M, L, K, in that order
  * case 3: if for both patterns L=M: compare L (or M), K
  * case 4: if one L1>M1, other L2=M2: compare L, K, M
  * case 5: if one L1<M1, other L2=M2: compare M, L, K
  * case 6: if one pattern L1>M1, the other M2>L2: first is more generic
     unless (case 6a) K1=K2=1,M1=0,M2=L2+1 (in that case - equal)

  note that in case 3 one can use a rule from the case either 1 or 2,
  in the case 4 one can use the rule from the case 1,
  in the case 5 one can use the rule from the case 2.

  for the case 6 and ignoring the special case 6a, to compare patterns by a
  magic number as a function z(a,b,c), we must ensure that z(L1,K1,M1) is
  greater than z(M2,L2,K2) when L1=M2. This can be done by an extra bit,
  which is 1 for K and 0 for L. Thus, the magic number could be

  case 1: (((L*2 + 1)*(maxlen+1) + K)*(maxlen+1) + M)*(maxlen+1) + P
  case 2: ((M*2*(maxlen+1) + L)*(maxlen+1) + K)*(maxlen+1) + P

  upper bound: L<=maxlen, M<=maxlen, K<=maxlen/2, P<maxlen
  for a current maxlen=64, the magic number needs magic_bits bits.
*/

static ulonglong get_magic_sort(const char *templ, ...)
{
  ulonglong sort=0;
  va_list args;
  va_start(args, templ);

  IF_DBUG(uint bits_used= 0,);

  for (; *templ; templ++)
  {
    char *pat= va_arg(args, char*);

    if (*templ == 'u')
    {
      /* Username. Can be empty (= anybody) or a literal. Encoded in one bit */
      sort= (sort << 1) + !*pat;
      IF_DBUG(bits_used++,);
      continue;
    }

    /* A wildcard pattern.  Encoded in magic_bits bits.  */
    uint maxlen= *templ == 'd' ? max_dbname_length : max_hostname_length;
    DBUG_ASSERT(maxlen <= 255);
    DBUG_ASSERT(*templ == 'd' || *templ == 'h');

    uint N= 0, M= 0, K= 0, P= 0;
    for (uint i=0; pat[i]; i++)
    {
      if (pat[i] == wild_many)
      {
        if (!K && !M) P= N;
        K++;
        while (pat[i+1] == wild_many) i++;
        continue;
      }
      if (pat[i] == wild_one)
      {
        if (!K && !M) P= N;
        M++;
        continue;
      }
      if (pat[i] == wild_prefix && pat[i+1]) i++;
      N++;
    }

    set_if_smaller(K, 31);
    set_if_smaller(M, 31);

    ulonglong L= K ? maxlen - N - M : 0, d= maxlen + 1, magic;
    ulonglong d1= MY_MIN(d, 32);
    if (L > M)
      magic= (((L * 2 + 1) * d + K) * d1 + M) * d + P;
    else
      magic= (((M * 2 + 0) * d + L) * d1 + K) * d + P;
    DBUG_ASSERT(magic < (1ULL << magic_bits));
    sort= (sort << magic_bits) + magic;
    IF_DBUG(bits_used+= magic_bits,);
  }
  DBUG_ASSERT(bits_used < 8*sizeof(sort));
  va_end(args);
  return ~sort;
}
#endif
