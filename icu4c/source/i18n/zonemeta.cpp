/*
*******************************************************************************
* Copyright (C) 2007-2010, International Business Machines Corporation and    *
* others. All Rights Reserved.                                                *
*******************************************************************************
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "zonemeta.h"

#include "unicode/timezone.h"
#include "unicode/ustring.h"
#include "unicode/putil.h"

#include "umutex.h"
#include "uvector.h"
#include "cmemory.h"
#include "gregoimp.h"
#include "cstring.h"
#include "ucln_in.h"

// Metazone mapping tables
static UMTX gZoneMetaLock = NULL;
static UHashtable *gCanonicalMap = NULL;
static UHashtable *gOlsonToMeta = NULL;
static UBool gCanonicalMapInitialized = FALSE;
static UBool gOlsonToMetaInitialized = FALSE;

U_CDECL_BEGIN


/**
 * Cleanup callback func
 */
static UBool U_CALLCONV zoneMeta_cleanup(void)
{
     umtx_destroy(&gZoneMetaLock);

    if (gCanonicalMap != NULL) {
        uhash_close(gCanonicalMap);
        gCanonicalMap = NULL;
    }
    gCanonicalMapInitialized = FALSE;

    if (gOlsonToMeta != NULL) {
        uhash_close(gOlsonToMeta);
        gOlsonToMeta = NULL;
    }
    gOlsonToMetaInitialized = FALSE;

    return TRUE;
}

/**
 * Deleter for UChar* string
 */
static void U_CALLCONV
deleteUCharString(void *obj) {
    UChar *entry = (UChar*)obj;
    uprv_free(entry);
}

/**
 * Deleter for UVector
 */
static void U_CALLCONV
deleteUVector(void *obj) {
   delete (U_NAMESPACE_QUALIFIER UVector*) obj;
}

/**
 * Deleter for CanonicalMapEntry
 */
static void U_CALLCONV
deleteCanonicalMapEntry(void *obj) {
    U_NAMESPACE_QUALIFIER CanonicalMapEntry *entry = (U_NAMESPACE_QUALIFIER CanonicalMapEntry*)obj;
    uprv_free(entry);
}

/**
 * Deleter for OlsonToMetaMappingEntry
 */
static void U_CALLCONV
deleteOlsonToMetaMappingEntry(void *obj) {
    U_NAMESPACE_QUALIFIER OlsonToMetaMappingEntry *entry = (U_NAMESPACE_QUALIFIER OlsonToMetaMappingEntry*)obj;
    uprv_free(entry);
}

U_CDECL_END

U_NAMESPACE_BEGIN

#define ZID_KEY_MAX 128
static const char gSupplementalData[]   = "supplementalData";
static const char gMapTimezonesTag[]    = "mapTimezones";
static const char gZoneFormattingTag[]  = "zoneFormatting";
static const char gCanonicalTag[]       = "canonical";
static const char gTerritoryTag[]       = "territory";
static const char gAliasesTag[]         = "aliases";
static const char gMultizoneTag[]       = "multizone";

static const char gMetaZones[]          = "metaZones";
static const char gMetazoneInfo[]       = "metazoneInfo";

static const UChar gWorld[] = {0x30, 0x30, 0x31, 0x00}; // "001"

static const UChar gDefaultFrom[] = {0x31, 0x39, 0x37, 0x30, 0x2D, 0x30, 0x31, 0x2D, 0x30, 0x31,
                                     0x20, 0x30, 0x30, 0x3A, 0x30, 0x30, 0x00}; // "1970-01-01 00:00"
static const UChar gDefaultTo[]   = {0x39, 0x39, 0x39, 0x39, 0x2D, 0x31, 0x32, 0x2D, 0x33, 0x31,
                                     0x20, 0x32, 0x33, 0x3A, 0x35, 0x39, 0x00}; // "9999-12-31 23:59"

#define ASCII_DIGIT(c) (((c)>=0x30 && (c)<=0x39) ? (c)-0x30 : -1)

/*
 * Convert a date string used by metazone mappings to UDate.
 * The format used by CLDR metazone mapping is "yyyy-MM-dd HH:mm".
 */
static UDate
parseDate (const UChar *text, UErrorCode &status) {
    if (U_FAILURE(status)) {
        return 0;
    }
    int32_t len = u_strlen(text);
    if (len != 16 && len != 10) {
        // It must be yyyy-MM-dd HH:mm (length 16) or yyyy-MM-dd (length 10)
        status = U_INVALID_FORMAT_ERROR;
        return 0;
    }

    int32_t year = 0, month = 0, day = 0, hour = 0, min = 0, n;
    int32_t idx;

    // "yyyy" (0 - 3)
    for (idx = 0; idx <= 3 && U_SUCCESS(status); idx++) {
        n = ASCII_DIGIT((int32_t)text[idx]);
        if (n >= 0) {
            year = 10*year + n;
        } else {
            status = U_INVALID_FORMAT_ERROR;
        }
    }
    // "MM" (5 - 6)
    for (idx = 5; idx <= 6 && U_SUCCESS(status); idx++) {
        n = ASCII_DIGIT((int32_t)text[idx]);
        if (n >= 0) {
            month = 10*month + n;
        } else {
            status = U_INVALID_FORMAT_ERROR;
        }
    }
    // "dd" (8 - 9)
    for (idx = 8; idx <= 9 && U_SUCCESS(status); idx++) {
        n = ASCII_DIGIT((int32_t)text[idx]);
        if (n >= 0) {
            day = 10*day + n;
        } else {
            status = U_INVALID_FORMAT_ERROR;
        }
    }
    if (len == 16) {
        // "HH" (11 - 12)
        for (idx = 11; idx <= 12 && U_SUCCESS(status); idx++) {
            n = ASCII_DIGIT((int32_t)text[idx]);
            if (n >= 0) {
                hour = 10*hour + n;
            } else {
                status = U_INVALID_FORMAT_ERROR;
            }
        }
        // "mm" (14 - 15)
        for (idx = 14; idx <= 15 && U_SUCCESS(status); idx++) {
            n = ASCII_DIGIT((int32_t)text[idx]);
            if (n >= 0) {
                min = 10*min + n;
            } else {
                status = U_INVALID_FORMAT_ERROR;
            }
        }
    }

    if (U_SUCCESS(status)) {
        UDate date = Grego::fieldsToDay(year, month - 1, day) * U_MILLIS_PER_DAY
            + hour * U_MILLIS_PER_HOUR + min * U_MILLIS_PER_MINUTE;
        return date;
    }
    return 0;
}

UHashtable*
ZoneMeta::createCanonicalMap(void) {
    UErrorCode status = U_ZERO_ERROR;

    UHashtable *canonicalMap = NULL;
    UResourceBundle *zoneFormatting = NULL;
    UResourceBundle *tzitem = NULL;
    UResourceBundle *aliases = NULL;

    UResourceBundle *idArray = NULL;

    canonicalMap = uhash_open(uhash_hashUChars, uhash_compareUChars, NULL, &status);
    if (U_FAILURE(status)) {
        return NULL;
    }
    // no key deleter
    uhash_setValueDeleter(canonicalMap, deleteCanonicalMapEntry);

    zoneFormatting = ures_openDirect(NULL, gSupplementalData, &status);
    zoneFormatting = ures_getByKey(zoneFormatting, gZoneFormattingTag, zoneFormatting, &status);
    if (U_FAILURE(status)) {
        goto error_cleanup;
    }

    while (ures_hasNext(zoneFormatting)) {
        tzitem = ures_getNextResource(zoneFormatting, tzitem, &status);
        if (U_FAILURE(status)) {
            status = U_ZERO_ERROR;
            continue;
        }
        if (ures_getType(tzitem) != URES_TABLE) {
            continue;
        }

        int32_t canonicalLen;
        const UChar *canonical = ures_getStringByKey(tzitem, gCanonicalTag, &canonicalLen, &status);
        if (U_FAILURE(status)) {
            status = U_ZERO_ERROR;
            continue;
        }

        int32_t territoryLen;
        const UChar *territory = ures_getStringByKey(tzitem, gTerritoryTag, &territoryLen, &status);
        if (U_FAILURE(status)) {
            territory = NULL;
            status = U_ZERO_ERROR;
        }

        // Create canonical map entry
        CanonicalMapEntry *entry = (CanonicalMapEntry*)uprv_malloc(sizeof(CanonicalMapEntry));
        if (entry == NULL) {
            status = U_MEMORY_ALLOCATION_ERROR;
            goto error_cleanup;
        }
        entry->id = canonical;
        if (territory == NULL || u_strcmp(territory, gWorld) == 0) {
            entry->country = NULL;
        } else {
            entry->country = territory;
        }

        // Put this entry in the hashtable. Since this hashtable has no key deleter,
        // key is treated as const, but must be passed as non-const.
        uhash_put(canonicalMap, (UChar*)canonical, entry, &status);
        if (U_FAILURE(status)) {
            goto error_cleanup;
        }

        // Get aliases
        aliases = ures_getByKey(tzitem, gAliasesTag, aliases, &status);
        if (U_FAILURE(status)) {
            // No aliases
            status = U_ZERO_ERROR;
            continue;
        }

        while (ures_hasNext(aliases)) {
            const UChar* alias = ures_getNextString(aliases, NULL, NULL, &status);
            if (U_FAILURE(status)) {
                status = U_ZERO_ERROR;
                continue;
            }
            // Create canonical map entry for this alias
            entry = (CanonicalMapEntry*)uprv_malloc(sizeof(CanonicalMapEntry));
            if (entry == NULL) {
                status = U_MEMORY_ALLOCATION_ERROR;
                goto error_cleanup;
            }
            entry->id = canonical;
            if (territory  == NULL || u_strcmp(territory, gWorld) == 0) {
                entry->country = NULL;
            } else {
                entry->country = territory;
            }

            // Put this entry in the hashtable. Since this hashtable has no key deleter,
            // key is treated as const, but must be passed as non-const.
            uhash_put(canonicalMap, (UChar*)alias, entry, &status);
            if (U_FAILURE(status)) {
                goto error_cleanup;
            }
        }
    }

    // Some available Olson zones are not included in CLDR data (such as Asia/Riyadh87).
    // Also, when we update Olson tzdata, new zones may be added.
    // This code scans all available zones in zoneinfo.res, and if any of them are
    // missing, add them to the map.
    idArray = TimeZone::getIDArray(status);
    if (U_SUCCESS(status)) {
        const UChar *zone;
        int32_t numZones = ures_getSize(idArray);
        int32_t i;
        UnicodeString zoneStr;
        for (i = 0; i < numZones; i++) {
            zone = ures_getStringByIndex(idArray, i, NULL, &status);
            if (U_FAILURE(status)) {
                // ignore this
                status = U_ZERO_ERROR;
                continue;
            }
            CanonicalMapEntry *entry = (CanonicalMapEntry*)uhash_get(canonicalMap, zone);
            if (entry) {
                // Already included in CLDR data
                continue;
            }

            zoneStr.setTo(zone, -1);

            // Not in CLDR data, but it could be new one whose alias is available
            // in CLDR.
            int32_t nTzdataEquivalent = TimeZone::countEquivalentIDs(zoneStr);
            int32_t j;
            for (j = 0; j < nTzdataEquivalent; j++) {
                UnicodeString alias = TimeZone::getEquivalentID(zoneStr, j);
                if (alias == zoneStr) {
                    continue;
                }
                UChar aliasUChars[ZID_KEY_MAX];
                alias.extract(aliasUChars, ZID_KEY_MAX, status);
                if (U_FAILURE(status) || status==U_STRING_NOT_TERMINATED_WARNING) {
                    status = U_ZERO_ERROR;
                    continue; // zone id is too long to extract
                }
                entry = (CanonicalMapEntry*)uhash_get(canonicalMap, aliasUChars);
                if (entry != NULL) {
                    break;
                }
            }
            // Create a new map entry
            CanonicalMapEntry* newEntry = (CanonicalMapEntry*)uprv_malloc(sizeof(CanonicalMapEntry));
            if (newEntry == NULL) {
                status = U_MEMORY_ALLOCATION_ERROR;
                goto error_cleanup;
            }
            if (entry == NULL) {
                // Set dereferenced zone ID as the canonical ID
                const UChar* derefZone = TimeZone::dereferOlsonLink(zoneStr);
                if (derefZone == NULL) {
                    // it should never happen..
                    delete newEntry;
                    continue;
                }
                newEntry->id = derefZone;

                // No territory information available
                newEntry->country = NULL;
            } else {
                // Duplicate the entry
                newEntry->id = entry->id;
                newEntry->country = entry->country;
            }

            // Put this entry in the hashtable.
            // key is treated as const, but must be passed as non-const.
            uhash_put(canonicalMap, (UChar*)zone, newEntry, &status);
            if (U_FAILURE(status)) {
                goto error_cleanup;
            }
        }
    }

normal_cleanup:
    ures_close(aliases);
    ures_close(tzitem);
    ures_close(zoneFormatting);
    ures_close(idArray);
    return canonicalMap;

error_cleanup:
    if (canonicalMap != NULL) {
        uhash_close(canonicalMap);
        canonicalMap = NULL;
    }
    goto normal_cleanup;
}

 /*
 * Initialize global objects
 */
void
ZoneMeta::initializeCanonicalMap(void) {
    UBool initialized;
    UMTX_CHECK(&gZoneMetaLock, gCanonicalMapInitialized, initialized);
    if (initialized) {
        return;
    }
    // Initialize hash table
    UHashtable *tmpCanonicalMap = createCanonicalMap();

    umtx_lock(&gZoneMetaLock);
    if (!gCanonicalMapInitialized) {
        gCanonicalMap = tmpCanonicalMap;
        tmpCanonicalMap = NULL;
        gCanonicalMapInitialized = TRUE;
    }
    umtx_unlock(&gZoneMetaLock);

    // OK to call the following multiple times with the same function
    ucln_i18n_registerCleanup(UCLN_I18N_ZONEMETA, zoneMeta_cleanup);
    if (tmpCanonicalMap != NULL) {
        uhash_close(tmpCanonicalMap);
    }
}

UnicodeString& U_EXPORT2
ZoneMeta::getCanonicalSystemID(const UnicodeString &tzid, UnicodeString &systemID, UErrorCode& status) {
    const CanonicalMapEntry *entry = getCanonicalInfo(tzid);
    if (entry != NULL) {
        systemID.setTo(entry->id);
    } else {
        status = U_ILLEGAL_ARGUMENT_ERROR;
    }
    return systemID;
}

UnicodeString& U_EXPORT2
ZoneMeta::getCanonicalCountry(const UnicodeString &tzid, UnicodeString &canonicalCountry) {
    const CanonicalMapEntry *entry = getCanonicalInfo(tzid);
    if (entry != NULL && entry->country != NULL) {
        canonicalCountry.setTo(entry->country);
    } else {
        // Use the input tzid
        canonicalCountry.remove();
    }
    return canonicalCountry;
}

const CanonicalMapEntry* U_EXPORT2
ZoneMeta::getCanonicalInfo(const UnicodeString &tzid) {
    initializeCanonicalMap();
    CanonicalMapEntry *entry = NULL;
    if (gCanonicalMap != NULL) {
        UErrorCode status = U_ZERO_ERROR;
        UChar tzidUChars[ZID_KEY_MAX];
        tzid.extract(tzidUChars, ZID_KEY_MAX, status);
        if (U_SUCCESS(status) && status!=U_STRING_NOT_TERMINATED_WARNING) {
            entry = (CanonicalMapEntry*)uhash_get(gCanonicalMap, tzidUChars);
        }
    }
    return entry;
}

UnicodeString& U_EXPORT2
ZoneMeta::getSingleCountry(const UnicodeString &tzid, UnicodeString &country) {
    UErrorCode status = U_ZERO_ERROR;

    // Get canonical country for the zone
    getCanonicalCountry(tzid, country);

    if (!country.isEmpty()) { 
        UResourceBundle *supplementalDataBundle = ures_openDirect(NULL, gSupplementalData, &status);
        UResourceBundle *zoneFormatting = ures_getByKey(supplementalDataBundle, gZoneFormattingTag, NULL, &status);
        UResourceBundle *multizone = ures_getByKey(zoneFormatting, gMultizoneTag, NULL, &status);

        if (U_SUCCESS(status)) {
            while (ures_hasNext(multizone)) {
                int32_t len;
                const UChar* multizoneCountry = ures_getNextString(multizone, &len, NULL, &status);
                if (country.compare(multizoneCountry, len) == 0) {
                    // Included in the multizone country list
                    country.remove();
                    break;
                }
            }
        }

        ures_close(multizone);
        ures_close(zoneFormatting);
        ures_close(supplementalDataBundle);
    }

    return country;
}

UnicodeString& U_EXPORT2
ZoneMeta::getMetazoneID(const UnicodeString &tzid, UDate date, UnicodeString &result) {
    UBool isSet = FALSE;
    const UVector *mappings = getMetazoneMappings(tzid);
    if (mappings != NULL) {
        for (int32_t i = 0; i < mappings->size(); i++) {
            OlsonToMetaMappingEntry *mzm = (OlsonToMetaMappingEntry*)mappings->elementAt(i);
            if (mzm->from <= date && mzm->to > date) {
                result.setTo(mzm->mzid, -1);
                isSet = TRUE;
                break;
            }
        }
    }
    if (!isSet) {
        result.remove();
    }
    return result;
}

const UVector* U_EXPORT2
ZoneMeta::getMetazoneMappings(const UnicodeString &tzid) {
    UErrorCode status = U_ZERO_ERROR;
    UChar tzidUChars[ZID_KEY_MAX];
    tzid.extract(tzidUChars, ZID_KEY_MAX, status);
    if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING) {
        return NULL;
    }

    UBool initialized;
    UMTX_CHECK(&gZoneMetaLock, gOlsonToMetaInitialized, initialized);
    if (!initialized) {
        UHashtable *tmpOlsonToMeta = uhash_open(uhash_hashUChars, uhash_compareUChars, NULL, &status);
        if (U_FAILURE(status)) {
            return NULL;
        }
        uhash_setKeyDeleter(tmpOlsonToMeta, deleteUCharString);
        uhash_setValueDeleter(tmpOlsonToMeta, deleteUVector);

        umtx_lock(&gZoneMetaLock);
        {
            if (!gOlsonToMetaInitialized) {
                gOlsonToMeta = tmpOlsonToMeta;
                tmpOlsonToMeta = NULL;
                gOlsonToMetaInitialized = TRUE;
            }
        }
        umtx_unlock(&gZoneMetaLock);

        // OK to call the following multiple times with the same function
        ucln_i18n_registerCleanup(UCLN_I18N_ZONEMETA, zoneMeta_cleanup);
        if (tmpOlsonToMeta != NULL) {
            uhash_close(tmpOlsonToMeta);
        }
    }

    // get the mapping from cache
    const UVector *result = NULL;

    umtx_lock(&gZoneMetaLock);
    {
        result = (UVector*) uhash_get(gOlsonToMeta, tzidUChars);
    }
    umtx_unlock(&gZoneMetaLock);

    if (result != NULL) {
        return result;
    }

    // miss the cache - create new one
    UVector *tmpResult = createMetazoneMappings(tzid);
    if (tmpResult == NULL) {
        // not available
        return NULL;
    }

    // put the new one into the cache
    umtx_lock(&gZoneMetaLock);
    {
        // make sure it's already created
        result = (UVector*) uhash_get(gOlsonToMeta, tzidUChars);
        if (result == NULL) {
            // add the one just created
            int32_t tzidLen = tzid.length() + 1;
            UChar *key = (UChar*)uprv_malloc(tzidLen * sizeof(UChar));
            if (key == NULL) {
                // memory allocation error..  just return NULL
                result = NULL;
                delete tmpResult;
            } else {
                tzid.extract(key, tzidLen, status);
                uhash_put(gOlsonToMeta, key, tmpResult, &status);
                if (U_FAILURE(status)) {
                    // delete the mapping
                    result = NULL;
                    delete tmpResult;
                } else {
                    result = tmpResult;
                }
            }
        } else {
            // another thread already put the one
            delete tmpResult;
        }
    }
    umtx_unlock(&gZoneMetaLock);

    return result;
}

UVector*
ZoneMeta::createMetazoneMappings(const UnicodeString &tzid) {
    UVector *mzMappings = NULL;
    UErrorCode status = U_ZERO_ERROR;

    UnicodeString canonicalID;
    UResourceBundle *rb = ures_openDirect(NULL, gMetaZones, &status);
    ures_getByKey(rb, gMetazoneInfo, rb, &status);
    TimeZone::getCanonicalID(tzid, canonicalID, status);

    if (U_SUCCESS(status)) {
        char tzKey[ZID_KEY_MAX];
        canonicalID.extract(0, canonicalID.length(), tzKey, sizeof(tzKey), US_INV);

        // tzid keys are using ':' as separators
        char *p = tzKey;
        while (*p) {
            if (*p == '/') {
                *p = ':';
            }
            p++;
        }

        ures_getByKey(rb, tzKey, rb, &status);

        if (U_SUCCESS(status)) {
            UResourceBundle *mz = NULL;
            while (ures_hasNext(rb)) {
                mz = ures_getNextResource(rb, mz, &status);

                const UChar *mz_name = ures_getStringByIndex(mz, 0, NULL, &status);
                const UChar *mz_from = gDefaultFrom;
                const UChar *mz_to = gDefaultTo;

                if (ures_getSize(mz) == 3) {
                    mz_from = ures_getStringByIndex(mz, 1, NULL, &status);
                    mz_to   = ures_getStringByIndex(mz, 2, NULL, &status);
                }

                if(U_FAILURE(status)){
                    status = U_ZERO_ERROR;
                    continue;
                }
                // We do not want to use SimpleDateformat to parse boundary dates,
                // because this code could be triggered by the initialization code
                // used by SimpleDateFormat.
                UDate from = parseDate(mz_from, status);
                UDate to = parseDate(mz_to, status);
                if (U_FAILURE(status)) {
                    status = U_ZERO_ERROR;
                    continue;
                }

                OlsonToMetaMappingEntry *entry = (OlsonToMetaMappingEntry*)uprv_malloc(sizeof(OlsonToMetaMappingEntry));
                if (entry == NULL) {
                    status = U_MEMORY_ALLOCATION_ERROR;
                    break;
                }
                entry->mzid = mz_name;
                entry->from = from;
                entry->to = to;

                if (mzMappings == NULL) {
                    mzMappings = new UVector(deleteOlsonToMetaMappingEntry, NULL, status);
                    if (U_FAILURE(status)) {
                        delete mzMappings;
                        deleteOlsonToMetaMappingEntry(entry);
                        uprv_free(entry);
                        break;
                    }
                }

                mzMappings->addElement(entry, status);
                if (U_FAILURE(status)) {
                    break;
                }
            }
            ures_close(mz);
            if (U_FAILURE(status)) {
                if (mzMappings != NULL) {
                    delete mzMappings;
                    mzMappings = NULL;
                }
            }
        }
    }
    ures_close(rb);
    return mzMappings;
}

UnicodeString& U_EXPORT2
ZoneMeta::getZoneIdByMetazone(const UnicodeString &mzid, const UnicodeString &region, UnicodeString &result) {

    char *pRegionSuffix = NULL;
    char mzidChars[ZID_KEY_MAX + 4];
    int32_t mzLen = mzid.extract(0, mzid.length(), mzidChars, ZID_KEY_MAX, US_INV);

    if (region.length() ==2 || region.length() == 3) {
        pRegionSuffix = &mzidChars[mzLen];
        *pRegionSuffix = ':';
        region.extract(0, region.length(), &mzidChars[mzLen + 1], sizeof(mzidChars) - mzLen, US_INV);
    }

    UErrorCode status = U_ZERO_ERROR;
    const UChar *tzid = NULL;
    int32_t tzidLen = 0;

    UResourceBundle *rb = ures_openDirect(NULL, gMetaZones, &status);
    ures_getByKey(rb, gMapTimezonesTag, rb, &status);
    if (U_SUCCESS(status)) {
        tzid = ures_getStringByKey(rb, mzidChars, &tzidLen, &status);
        if (status == U_MISSING_RESOURCE_ERROR && pRegionSuffix != NULL) {
            status = U_ZERO_ERROR;
            // try key without region
            *pRegionSuffix = 0;
            tzid = ures_getStringByKey(rb, mzidChars, &tzidLen, &status);
        }
    }
    ures_close(rb);

    if (tzidLen > 0) {
        result.setTo(tzid, tzidLen);
    } else {
        result.remove();
    }

    return result;
}


U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */
