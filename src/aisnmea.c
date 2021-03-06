/*  =========================================================================
    aisnmea - AIS NMEA sentence parser class

    Copyright (c) 2017 Inkblot Software Limited.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.
    =========================================================================
*/

/*
@header
    aisnmea -
@discuss
@end
*/

#include "aisnmea_classes.h"

//  Structure of our class

struct _aisnmea_t {
    // Parsed tagblock, or NULL if no tagblock found
    zhash_t *tagblock_data;

    // Core NMEA cols
    char *head;
    size_t fragcount;
    size_t fragnum;
    int messageid;  // -1 is sentinel for col empty
    char channel;   // -1 is sentinel for col empty
    char *payload;
    size_t fillbits;
    size_t checksum;
};


//  --------------------------------------------------------------------------
//  For debugging

#define logg(str, ...) do {  \
                           zsys_init ();  \
                           zsys_debug ("------------------------------------"); \
                           zsys_debug ("++++++ %s:%d:1 " str, __FILE__, __LINE__,  \
                                                         ##__VA_ARGS__);   \
                       } while (0);

#define log(str) logg(str, NULL);


//  --------------------------------------------------------------------------
//  Forward declare static helpers

static int
s_setfrom_innernmea (aisnmea_t *self, const char *inner_nmea);

static int
s_setfrom_nmeawithtagblock (aisnmea_t *self, zlist_t *outercols);

static zhash_t *
s_parse_tagblock (const char *tagblock);

static zlist_t *
s_delimstring_split (const char *string, char delim);

static int
s_calc_checksum (const char *str);


//  --------------------------------------------------------------------------
//  Create a new aisnmea, parsing nmea and storing its data internally.
//    If you only want to create an aisnmea and use it for parsing later, pass
//    NULL in place of nmea.

aisnmea_t *
aisnmea_new (const char *nmea)
{
    aisnmea_t *self = (aisnmea_t *) zmalloc (sizeof (aisnmea_t));
    assert (self);

    // Default construct if NULL string given
    if (!nmea)
        return self;

    int err = aisnmea_parse (self, nmea);
    if (err)
        goto die;
    
    return self;

 die:
    aisnmea_destroy (&self);
    return NULL;
}


//  --------------------------------------------------------------------------
//  Destroy the aisnmea

void
aisnmea_destroy (aisnmea_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        aisnmea_t *self = *self_p;

        zstr_free (&self->head);
        zstr_free (&self->payload);
        
        zhash_destroy (&self->tagblock_data);

        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Object duplicator

aisnmea_t *
aisnmea_dup (aisnmea_t *self) {
    assert (self);

    aisnmea_t *res = aisnmea_new (NULL);
    assert (res);

    if (self->tagblock_data) {
        res->tagblock_data = zhash_new();
        assert (res->tagblock_data);
        zhash_autofree (res->tagblock_data);

        // Iterate hashtable in self, copying values into res hashtable
        char *tb_val = (char *) zhash_first (self->tagblock_data);
        while (tb_val) {
            const char *tb_key = (const char *) zhash_cursor (self->tagblock_data);

            // NB autofree means that vals are copied; keys are always copied
            int rc = zhash_insert (res->tagblock_data, tb_key, tb_val);
            assert (!rc);
        
            tb_val = (char *) zhash_next (self->tagblock_data);
        }
    }

    res->head      = strdup (self->head);
    res->fragcount = self->fragcount;
    res->fragnum   = self->fragnum;
    res->messageid = self->messageid;
    res->channel   = self->channel;
    res->payload   = strdup (self->payload);
    res->fillbits  = self->fillbits;
    res->checksum  = self->checksum;

    return res;
}


//  --------------------------------------------------------------------------
//  Parse a full AIS NMEA line, and store its data in self.
//  Returns 0 on succes, -1 on failure

int
aisnmea_parse (aisnmea_t *self, const char *nmea)
{
    assert (self);
    assert (nmea);
    zhash_destroy (&self->tagblock_data);

    int ret = -1;  // assume failed unless succeeded

    zlist_t *outercols = s_delimstring_split (nmea, '\\');
    if (zlist_size (outercols) == 3)
        ret = s_setfrom_nmeawithtagblock (self, outercols);
    else
    if (zlist_size (outercols) == 1)
        ret = s_setfrom_innernmea (self, nmea);
    else
        ret = -1;  // no change

    zlist_destroy (&outercols);
    return ret;
}

    
//  --------------------------------------------------------------------------
//  AIS message type mapping to first payload character

typedef struct _ais_typemap_s {
    char bodychar;
    int aistype;
} ais_typemap_s;

static const ais_typemap_s
s_ais_typemaps[] = { {'1', 1},  {'2', 2},  {'3', 3},  {'4', 4},
                     {'5', 5},  {'6', 6},  {'7', 7},  {'8', 8},
                     {'9', 9},  {':', 10}, {';', 11}, {'<', 12},
                     {'=', 13}, {'>', 14}, {'?', 15}, {'@', 16},
                     {'A', 17}, {'B', 18}, {'C', 19}, {'D', 20},
                     {'E', 21}, {'F', 22}, {'G', 23}, {'H', 24},
                     {'I', 25}, {'J', 26}, {'K', 27}, {'L', 28},
                     {0, -1} };  // sentinel

// Returns -1 if type no known
static int
s_ais_msgtype_fromchar (int ch)
{
    const ais_typemap_s *t = s_ais_typemaps;
    while (t->bodychar) {
        if (t->bodychar == ch)
            return t->aistype;
        ++t;
    }
    return -1;
}


//  --------------------------------------------------------------------------
//  Parsing an NMEA tagblock string e.g. "a:bb,ccc:d*" to a zhash.
//  Returns NULL if parse fails.

static zhash_t *
s_parse_tagblock (const char *tagblock)
{
    assert (tagblock);

    zhash_t *res = zhash_new ();
    zhash_autofree (res);

    zlist_t *outercols = NULL;  // for removing the '*' checksum
    zlist_t *kv_pairs = NULL;   // for splitting into "kk:vv" parts

    // Remove checksum part
    outercols = s_delimstring_split (tagblock, '*');
    if (zlist_size (outercols) != 2)
        goto die;

    // Body of both cols
    const char *tagblock_dats = (const char *) zlist_first (outercols);
    const char *given_checksum_str = (const char *) zlist_next (outercols);

    // Read the checksum string part of tb, so we can compare to calc'd value later
    errno = 0;
    int given_checksum = strtol (given_checksum_str, NULL, 16);
    if (errno)
        goto die;
    
    // Check its checksum matches the one we parsed out above
    int actual_checksum = s_calc_checksum (tagblock_dats);
    if (actual_checksum != given_checksum)
        goto die;

    // All the individual k:v pairs (but not split on ':')
    kv_pairs = s_delimstring_split (tagblock_dats, ',');
    if (!kv_pairs)
        goto die;

    // Process each k:v pair in the list, storing as we go
    const char *cur_pair = (const char *) zlist_first (kv_pairs);
    while (cur_pair != NULL) {
        
        zlist_t *cur_pair_parts = s_delimstring_split (cur_pair, ':');
        if (zlist_size (cur_pair_parts) != 2)
            goto die;

        const char *key = (const char *) zlist_first (cur_pair_parts);
        char       *val =       (char *) zlist_next (cur_pair_parts);

        if (strlen (key) == 0 || strlen (val) == 0)
            goto die;
        
        int err = zhash_insert (res, key, val);
        assert (!err);

        zlist_destroy (&cur_pair_parts);
        cur_pair = (const char *) zlist_next (kv_pairs);
    }

    goto cleanup_return;
    
 die:
    zhash_destroy (&res);

 cleanup_return:
    zlist_destroy (&outercols);
    zlist_destroy (&kv_pairs);
    
    return res;
}    


//  --------------------------------------------------------------------------
//  Set self by parsing the 'inner nmea' (whole sentence minus any tagblock)
//     Returns 0 on success, -1 on parse error.
//     On error, self is left in an undefined state.

static int
s_setfrom_innernmea (aisnmea_t *self, const char *inner_nmea)
{
    assert (self);
    assert (inner_nmea);
    
    zlist_t *body_and_checksum = NULL;
    zlist_t *cols = NULL;

    body_and_checksum = s_delimstring_split (inner_nmea, '*');
    if (zlist_size (body_and_checksum) != 2)
        goto die;

    // TODO verify checksum
    const char *nm_body = (const char *) zlist_first (body_and_checksum);
    const char *checksum = (const char *) zlist_next (body_and_checksum);

    cols = s_delimstring_split (nm_body, ',');
    if (zlist_size (cols) != 7)
        goto die;

    // Col 1
    zstr_free (&self->head);
    self->head = strdup ((const char *) zlist_first (cols));

    // Col 2
    errno = 0;
    self->fragcount = strtol ((const char *) zlist_next (cols), NULL, 10);
    if (errno)
        goto die;

    // Col 3
    errno = 0;
    self->fragnum = strtol ((const char *) zlist_next (cols), NULL, 10);
    if (errno)
        goto die;

    // Col 4
    const char *mid_str = (const char *) zlist_next (cols);
    if (strlen (mid_str) == 0)
        self->messageid = -1;
    else {
        errno = 0;
        self->messageid = strtol (mid_str, NULL, 10);
        if (errno)
            goto die;
    }

    // Col 5
    const char *chan_str = (const char *) zlist_next (cols);
    if (strlen (chan_str) == 1)
        self->channel = chan_str[0];
    else
    if (strlen (chan_str) == 0)
        self->channel = -1;
    else
        goto die;

    // Col 6
    zstr_free (&self->payload);
    self->payload = strdup ((const char *) zlist_next (cols));

    // Col 7
    errno = 0;
    self->fillbits = strtol ((const char *) zlist_next (cols), NULL, 10);
    if (errno)
        goto die;

    // Checksum
    errno = 0;
    self->checksum = strtol (checksum, NULL, 16);
    if (errno)
        goto die;

    // Check checksum is right
    int actual_checksum = s_calc_checksum (nm_body);
    if (actual_checksum != self->checksum)
        goto die;

    zlist_destroy (&cols);
    zlist_destroy (&body_and_checksum);
    return 0;

 die:
    zlist_destroy (&cols);
    zlist_destroy (&body_and_checksum);
    return -1;
}


//  --------------------------------------------------------------------------
//  Parsing the top-level NMEA with tagblock.
//    Returns 0 on success, or -1 on parse error.
//    Leaves self in undefined state on error.

static int
s_setfrom_nmeawithtagblock (aisnmea_t *self, zlist_t *outercols)
{
    assert (!self->tagblock_data);  // caller should have killed this
    assert (zlist_size (outercols) == 3);
    
    zhash_destroy (&self->tagblock_data);

    const char *dummycol = (const char *) zlist_first (outercols);
    if (strlen (dummycol) != 0)
        return -1;
    
    const char *tb_str = (const char *) zlist_next (outercols);
    self->tagblock_data = s_parse_tagblock (tb_str);
    if (!self->tagblock_data)
        return -1;

    const char *innernmea = (const char *) zlist_next (outercols);
    return s_setfrom_innernmea (self, innernmea);
}


//  --------------------------------------------------------------------------
//  String util: split deliminted string.
//    Returns a zlist_t (with autofree set) holding copies of
//    all the subsections of 'delim'-delimited 'string'.

static zlist_t *
s_delimstring_split (const char *string, char delim)
{
    assert (string);
    assert (delim);  // NULL isn't useful here

    zlist_t *res = zlist_new ();
    zlist_autofree (res);

    // Special case: no chars means no cols, ignoring implicit delims
    // to the right and left of 'string'
    if (!strlen (string))
        return res;

    // We'll copy vals into here as we go to save mallocs; it auto-stretches.
    zchunk_t *strbuf = zchunk_new (NULL, 0);
    assert (strbuf);

    // Points to the delim char ending the last field, or just before the start of the string
    const char *beg = string - 1;
    // Points to the delim char ending the current field
    const char *end = strchr (beg+1, delim);
    
    // At termination, both pointers are NULL (found end of string)
    while (beg || end) {
        zchunk_set (strbuf, NULL, 0);

        // 'end' is set to NULL by strchr when we run out of cols. 
        // If so, set it to the terminating NULL so we can grab the final val
        const char *val_end = (end == NULL)
                                  ? (string + strlen (string))
                                  : end;

        zchunk_extend (strbuf, beg+1, val_end - (beg+1));
        // zlist_append() expects to find a NULL terminated string
        char nul = 0;
        zchunk_extend (strbuf, &nul, 1);
        
        zlist_append (res, zchunk_data (strbuf));
        
        beg = end;
        if (beg)
            end = strchr (beg+1, delim);
    }
    zchunk_destroy (&strbuf);

    return res;
}


//  --------------------------------------------------------------------------
//  NMEA checksum calculations

static int
s_calc_checksum (const char *str)
{
    int res = 0;
    const char *cur_char = str;

    if (!strlen (str))
        return res;

    if (str[0] == '!' || str[0] == '$')
        ++cur_char;

    while (*cur_char) {
        res ^= *cur_char;
        ++cur_char;
    }
    return res;
}


//  ----------------------------------------------------------------------
//  Accessors

const char *
aisnmea_head (aisnmea_t *self)
{
    assert (self);
    return self->head;
}

size_t
aisnmea_fragcount (aisnmea_t *self)
{
    assert (self);
    return self->fragcount;
}

size_t
aisnmea_fragnum (aisnmea_t *self)
{
    assert (self);
    return self->fragnum;
}

int
aisnmea_messageid (aisnmea_t *self)
{
    assert (self);
    return self->messageid;
}

char
aisnmea_channel (aisnmea_t *self)
{
    assert (self);
    return self->channel;
}

const char *
aisnmea_payload (aisnmea_t *self)
{
    assert (self);
    return self->payload;
}

size_t
aisnmea_fillbits (aisnmea_t *self)
{
    assert (self);
    return self->fillbits;
}

size_t
aisnmea_checksum (aisnmea_t *self)
{
    assert (self);
    return self->checksum;
}

int
aisnmea_aismsgtype (aisnmea_t *self)
{
    assert (self);
    assert (strlen (self->payload));
    return s_ais_msgtype_fromchar (self->payload[0]);
}

const char *
aisnmea_tagblockval (aisnmea_t *self, const char *key)
{
    assert (self);
    if (!self->tagblock_data)
        return NULL;
    return (const char *) zhash_lookup (self->tagblock_data, key);
}


//  --------------------------------------------------------------------------
//  Self test of this class

void
aisnmea_test (bool verbose)
{
    printf (" * aisnmea: ");

    zsys_init ();

    //  @selftest
    //  Simple create/destroy test

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // src/selftest-ro; if your test creates filesystem objects, please
    // do so under src/selftest-rw. They are defined below along with a
    // usecase for the variables (assert) to make compilers happy.
    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    // The following pattern is suggested for C selftest code:
    //    char *filename = NULL;
    //    filename = zsys_sprintf ("%s/%s", SELFTEST_DIR_RO, "mytemplate.file");
    //    assert (filename);
    //    ... use the filename for I/O ...
    //    zstr_free (&filename);
    // This way the same filename variable can be reused for many subtests.
    //
    // Uncomment these to use C++ strings in C++ selftest code:
    //std::string str_SELFTEST_DIR_RO = std::string(SELFTEST_DIR_RO);
    //std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);
    //assert ( (str_SELFTEST_DIR_RO != "") );
    //assert ( (str_SELFTEST_DIR_RW != "") );
    // NOTE that for "char*" context you need (str_SELFTEST_DIR_RO + "/myfilename").c_str()


    // -- s_delimstring_split () tests

    zlist_t *d1 = s_delimstring_split (",aaa,,b,", ',');
    assert (d1);
    assert (zlist_size (d1) == 5);
    assert (streq (zlist_first (d1), ""));
    assert (streq (zlist_next (d1),  "aaa"));
    assert (streq (zlist_next (d1),  ""));
    assert (streq (zlist_next (d1),  "b"));
    assert (streq (zlist_next (d1),  ""));
    zlist_destroy (&d1);

    zlist_t *d2 = s_delimstring_split ("", ',');
    assert (zlist_size (d2) == 0);
    zlist_destroy (&d2);

    if (verbose)
        log ("### DID DELINSTRING TESTS");

    
    // -- msgtype mapping

    assert ( s_ais_msgtype_fromchar ('3') == 3);
    assert ( s_ais_msgtype_fromchar ('I') == 25);
    assert ( s_ais_msgtype_fromchar ('}') == -1);  // invalid type

    if (verbose)
        log ("### DID MSGTYPE MAPPING TESTS");


    // -- checksum calculations

    int cs1 = s_calc_checksum ("g:1-2-73874,n:157036,s:r003669945,c:1241544035");
    assert (cs1 == strtol ("4A", NULL, 16));

    int cs2 = s_calc_checksum ("!AIVDM,1,1,,B,15N4cJ`005Jrek0H@9n`DW5608EP,0");
    assert (cs2 = strtol ("13", NULL, 16));

    if (verbose)
        log ("### DID CHECKSUM CALC TESTS");
    
    
    // -- parsing whole tagblocks
    
    // eg1

    zhash_t *tbhash = s_parse_tagblock ("aa:bb,c:d,eeeeee:ffff*3D");
    
    assert (tbhash);
    assert (zhash_size (tbhash) == 3);

    assert (zhash_lookup (tbhash, "aa"));
    assert (streq (zhash_lookup (tbhash, "aa"),
                   "bb"));
            
    assert (zhash_lookup (tbhash, "c"));
    assert (streq (zhash_lookup (tbhash, "c"),
                   "d"));

    assert (zhash_lookup (tbhash, "eeeeee"));
    assert (streq (zhash_lookup (tbhash, "eeeeee"),
                   "ffff"));

    zhash_destroy (&tbhash);

    // eg2

    zhash_t *nulltbhash = s_parse_tagblock ("asdf,");
    assert (!nulltbhash);

    if (verbose)
        log ("### DID s_parse_tagblocks () tests");

    
    // -- nmea with tagblock

    const char *nmea_example_1 =
        "\\g:1-2-73874,n:157036,s:r003669945,c:1241544035*4A\\!AIVDM,1,1,,B,15N4cJ`005Jrek0H@9n`DW5608EP,0*13";
    
    aisnmea_t *msg1 = aisnmea_new (nmea_example_1);
    assert (msg1);

    // tagblock

    assert (aisnmea_tagblockval (msg1, "g"));
    assert (streq (aisnmea_tagblockval (msg1, "g"),
                   "1-2-73874"));

    assert (aisnmea_tagblockval (msg1, "n"));
    assert (streq (aisnmea_tagblockval (msg1, "n"),
                   "157036"));

    assert (streq (aisnmea_tagblockval (msg1, "s"),
                   "r003669945"));
                   
    assert (streq (aisnmea_tagblockval (msg1, "c"),
                   "1241544035"));
    
    // core
    
    assert (streq ("!AIVDM", aisnmea_head (msg1)));
    assert (   1 == aisnmea_fragcount (msg1));
    assert (   1 == aisnmea_fragnum (msg1));
    assert (  -1 == aisnmea_messageid (msg1));
    assert ( 'B' == aisnmea_channel (msg1));
    assert (streq ("15N4cJ`005Jrek0H@9n`DW5608EP", aisnmea_payload (msg1)));
    assert (   0 == aisnmea_fillbits (msg1));
    assert (0x13 == aisnmea_checksum (msg1));
    assert (   1 == aisnmea_aismsgtype (msg1));
    
    aisnmea_destroy (&msg1);

    if (verbose)
        log ("### DID FULL PARSE WITH TAGBLOCK TESTS");


    // -- nmea without tagblock

    const char *nmea_example_2 =
        "!AIVDM,2,1,3,B,55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53,0*3E";

    // Check the reusing-object parse works
    aisnmea_t *msg2 = aisnmea_new (NULL);
    int errm2 = aisnmea_parse (msg2, nmea_example_1); // from before
    assert (!errm2);
    errm2 = aisnmea_parse (msg2, nmea_example_2);
    assert (!errm2);
    
    assert (streq ("!AIVDM", aisnmea_head (msg2)));
    assert (2 == aisnmea_fragcount (msg2));
    assert (1 == aisnmea_fragnum (msg2));
    assert (3 == aisnmea_messageid (msg2));
    assert ('B' == aisnmea_channel (msg2));
    assert (streq ("55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53",
                   aisnmea_payload (msg2)));
    assert (0 == aisnmea_fillbits (msg2));
    assert (0x3E == aisnmea_checksum (msg2));
    assert (5 == aisnmea_aismsgtype (msg2));

    aisnmea_destroy (&msg2);

    if (verbose)
        log ("### DID FULL PARSE WITHOUT TAGBLOCK TESTS");

    
    // -- duff nmea

    aisnmea_t *badtry;

    badtry = aisnmea_new ("");
    assert (!badtry);
    
    badtry = aisnmea_new ("asdfasdfasdf");
    assert (!badtry);

    badtry = aisnmea_new ("\\aaa\\bbb");
    assert (!badtry);

    badtry = aisnmea_new ("\\\\");
    assert (!badtry);

    badtry = aisnmea_new ("a,b,c,d,e,f,g,h*CC");
    assert (!badtry);

    badtry = aisnmea_new ("*");
    assert (!badtry);

    // Bad checksum in tb
    badtry = aisnmea_new ("\\g:1-2-73874,n:157036,s:r003669945,c:1241544035*40"
                          "\\!AIVDM,1,1,,B,15N4cJ`005Jrek0H@9n`DW5608EP,0*13");
    assert (!badtry);

    // Bad checksum in body
    badtry = aisnmea_new ("\\g:1-2-73874,n:157036,s:r003669945,c:1241544035*4A"
                          "\\!AIVDM,1,1,,B,15N4cJ`005Jrek0H@9n`DW5608EP,0*19");
    assert (!badtry);
    
    // Back checksum in body
    badtry = aisnmea_new ("!AIVDM,2,1,3,B,55P5TL01VIaAL@7WKO@mBplU@<"
                          "PDhh000000001S;AJ::4A80?4i@E53,0*8E");
    assert (!badtry);


    // -- dup ctr
    {
        // -- with tagblock
        
        const char *dup_nmea_tb =
            "\\g:1-2-73874,n:157036,s:r003669945,c:1241544035*4A\\!AIVDM,1,1,,B,15N4cJ`005Jrek0H@9n`DW5608EP,0*13";
        
        aisnmea_t *m1 = aisnmea_new (dup_nmea_tb);
        assert (m1);

        aisnmea_t *m2 = aisnmea_dup (m1);
        assert (m2);

        // Destroy to check no dangling ownership
        aisnmea_destroy (&m1);
        
        // tagblock

        assert (aisnmea_tagblockval (m2, "g"));
        assert (streq (aisnmea_tagblockval (m2, "g"),
                       "1-2-73874"));

        assert (aisnmea_tagblockval (m2, "n"));
        assert (streq (aisnmea_tagblockval (m2, "n"),
                       "157036"));

        assert (streq (aisnmea_tagblockval (m2, "s"),
                       "r003669945"));
                   
        assert (streq (aisnmea_tagblockval (m2, "c"),
                       "1241544035"));
    
        // core
    
        assert (streq ("!AIVDM", aisnmea_head (m2)));
        assert (   1 == aisnmea_fragcount (m2));
        assert (   1 == aisnmea_fragnum (m2));
        assert (  -1 == aisnmea_messageid (m2));
        assert ( 'B' == aisnmea_channel (m2));
        assert (streq ("15N4cJ`005Jrek0H@9n`DW5608EP", aisnmea_payload (m2)));
        assert (   0 == aisnmea_fillbits (m2));
        assert (0x13 == aisnmea_checksum (m2));
        assert (   1 == aisnmea_aismsgtype (m2));

        aisnmea_destroy (&m2);

        
        // -- and without tagblock

        const char *dup_nmea_notb =
            "!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C";

        aisnmea_t *m3 = aisnmea_new (dup_nmea_notb);
        aisnmea_t *m4 = aisnmea_dup (m3);
        aisnmea_destroy (&m3);
        
        assert (NULL == aisnmea_tagblockval(m4, "ABC"));
        assert ('B' == aisnmea_channel(m4));
        assert (streq ("177KQJ5000G?tO`K>RA1wUbN0TKH", aisnmea_payload(m4)));
        
        aisnmea_destroy (&m4);
    }

    

    if (verbose)
        log ("### DID FULL PARSE OF BROKEN NMEA TESTS");
    
    //  @end
    printf ("OK\n");
}
