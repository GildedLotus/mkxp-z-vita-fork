// SPDX-License-Identifier: GPL-3.0-or-later
/* The installed static Ruby archive supplies this instead of dmyext.o.
 * ruby_options calls Init_ext after the load path has been initialized.
 * Keep lazy require semantics: registration must not run Ruby library code
 * before MRI has installed its builtins. */
void ruby_init_ext(const char *name, void (*init)(void));

#define REGISTER(feature, function) do { \
    extern void function(void); \
    ruby_init_ext(feature ".so", function); \
} while (0)

void Init_ext(void)
{
    REGISTER("zlib", Init_zlib);
    REGISTER("stringio", Init_stringio);
    REGISTER("strscan", Init_strscan);
    REGISTER("date_core", Init_date_core);
    REGISTER("digest", Init_digest);
    REGISTER("digest/md5", Init_md5);
    REGISTER("digest/sha1", Init_sha1);
    REGISTER("digest/sha2", Init_sha2);
    REGISTER("digest/rmd160", Init_rmd160);
    REGISTER("digest/bubblebabble", Init_bubblebabble);
    REGISTER("objspace", Init_objspace);
}
