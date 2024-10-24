/*
 * system_update.c
 *
 * Copyright (c) 2020-2024, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nxdumptool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <core/nxdt_utils.h>
#include <core/system_update.h>
#include <core/cnmt.h>

/* Global variables. */

static Mutex g_systemUpdateMutex = 0;
static bool g_systemUpdateInterfaceInit = false;

static TitleInfo *g_systemUpdateTitleInfo = NULL;
static NcaContext *g_systemUpdateNcaContext = NULL;
static ContentMetaContext g_systemUpdateCnmtContext = {0};

/* Function prototypes. */

static bool _systemUpdateInitializeDumpContext(SystemUpdateDumpContext *ctx);

static bool systemUpdateProcessContentMetaInfo(SystemUpdateDumpContext *ctx, const NcmContentMetaInfo *content_meta_info);
static bool systemUpdateProcessContentRecords(SystemUpdateDumpContext *ctx, TitleInfo *title_info);

static int systemUpdateNcaContextSortFunction(const void *a, const void *b);

NX_INLINE NcaContext *systemUpdateGetCurrentNcaContextFromDumpContext(SystemUpdateDumpContext *ctx);

bool systemUpdateInitialize(void)
{
    bool ret = false;

    SCOPED_LOCK(&g_systemUpdateMutex)
    {
        ret = g_systemUpdateInterfaceInit;
        if (ret) break;

        /* Get title info. */
        if (!(g_systemUpdateTitleInfo = titleGetTitleInfoEntryFromStorageByTitleId(NcmStorageId_BuiltInSystem, SYSTEM_UPDATE_TID)))
        {
            LOG_MSG_ERROR("Failed to get title info for SystemUpdate!");
            break;
        }

        /* Allocate memory for the SystemUpdate NCA context. */
        g_systemUpdateNcaContext = calloc(1, sizeof(NcaContext));
        if (!g_systemUpdateNcaContext)
        {
            LOG_MSG_ERROR("Failed to allocate memory for SystemUpdate NCA context!");
            goto end;
        }

        /* Initialize NCA context. */
        /* Don't allow invalid NCA signatures. */
        if (!ncaInitializeContext(g_systemUpdateNcaContext, g_systemUpdateTitleInfo->storage_id, 0, &(g_systemUpdateTitleInfo->meta_key), \
            titleGetContentInfoByTypeAndIdOffset(g_systemUpdateTitleInfo, NcmContentType_Meta, 0), NULL) || !g_systemUpdateNcaContext->valid_main_signature)
        {
            LOG_MSG_ERROR("Failed to initialize SystemUpdate Meta NCA context!");
            goto end;
        }

        /* Initialize Content Meta context. */
        ret = g_systemUpdateInterfaceInit = (cnmtInitializeContext(&g_systemUpdateCnmtContext, g_systemUpdateNcaContext) && \
                                             g_systemUpdateCnmtContext.packaged_header->content_meta_count && g_systemUpdateCnmtContext.content_meta_info);
        if (ret) break;

        LOG_MSG_ERROR("Failed to initialize Content Meta context for SystemUpdate Meta NCA \"%s\"!", g_systemUpdateNcaContext->content_id_str);

end:
        cnmtFreeContext(&g_systemUpdateCnmtContext);

        if (g_systemUpdateNcaContext)
        {
            free(g_systemUpdateNcaContext);
            g_systemUpdateNcaContext = NULL;
        }

        titleFreeTitleInfo(&g_systemUpdateTitleInfo);
    }

    return ret;
}

void systemUpdateExit(void)
{
    SCOPED_LOCK(&g_systemUpdateMutex)
    {
        /* Free Content Meta context. */
        cnmtFreeContext(&g_systemUpdateCnmtContext);

        /* Free NCA context. */
        if (g_systemUpdateNcaContext)
        {
            free(g_systemUpdateNcaContext);
            g_systemUpdateNcaContext = NULL;
        }

        /* Free TitleInfo entry. */
        titleFreeTitleInfo(&g_systemUpdateTitleInfo);

        /* Update flag. */
        g_systemUpdateInterfaceInit = false;
    }
}

bool systemUpdateInitializeDumpContext(SystemUpdateDumpContext *ctx)
{
    bool ret = false;
    SCOPED_LOCK(&g_systemUpdateMutex) ret = _systemUpdateInitializeDumpContext(ctx);
    return ret;
}

bool systemUpdateGetCurrentContentFileSizeFromDumpContext(SystemUpdateDumpContext *ctx, u64 *out_size)
{
    NcaContext *nca_ctx = systemUpdateGetCurrentNcaContextFromDumpContext(ctx);
    if (!nca_ctx || !out_size)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return false;
    }

    *out_size = nca_ctx->content_size;

    return true;
}

char *systemUpdateGetCurrentContentFileNameFromDumpContext(SystemUpdateDumpContext *ctx)
{
    NcaContext *nca_ctx = systemUpdateGetCurrentNcaContextFromDumpContext(ctx);
    if (!nca_ctx)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return NULL;
    }

    bool is_meta_nca = (nca_ctx->content_type == NcmContentType_Meta);
    size_t nca_filename_size = ((is_meta_nca ? NCA_HFS_META_NAME_LENGTH : NCA_HFS_REGULAR_NAME_LENGTH) + 1);

    char *nca_filename = calloc(nca_filename_size, sizeof(char));
    if (nca_filename)
    {
        snprintf(nca_filename, nca_filename_size, "%s%s", nca_ctx->content_id_str, is_meta_nca ? ".cnmt.nca" : ".nca");
    } else {
        LOG_MSG_ERROR("Failed to allocate 0x%lX-byte long filename for %s NCA \"%s\"! (title %016lX).", nca_filename_size, titleGetNcmContentTypeName(nca_ctx->content_type), \
                                                                                                        nca_ctx->content_id_str, nca_ctx->title_id);
    }

    return nca_filename;
}

bool systemUpdateReadCurrentContentFileFromDumpContext(SystemUpdateDumpContext *ctx, void *out, u64 read_size)
{
    NcaContext *nca_ctx = NULL;

    if (!(nca_ctx = systemUpdateGetCurrentNcaContextFromDumpContext(ctx)) || !out || !read_size || (ctx->cur_content_offset + read_size) > nca_ctx->content_size)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return false;
    }

    bool success = false;

    /* Read NCA data. */
    if (!ncaReadContentFile(nca_ctx, out, read_size, ctx->cur_content_offset))
    {
        LOG_MSG_ERROR("Failed to read %s NCA \"%s\"! (title %016lX).", titleGetNcmContentTypeName(nca_ctx->content_type), \
                                                                       nca_ctx->content_id_str, nca_ctx->title_id);
        goto end;
    }

    /* (Re-)initialize SHA-256 hash context, if needed. */
    if (!ctx->cur_content_offset) sha256ContextCreate(&(ctx->sha256_ctx));

    /* Update SHA-256 hash context. */
    sha256ContextUpdate(&(ctx->sha256_ctx), out, read_size);

    /* Update system update context. */
    ctx->cur_size += read_size;
    ctx->cur_content_offset += read_size;

    /* Check if we have finished reading this content. */
    if (ctx->cur_content_offset >= nca_ctx->content_size)
    {
        /* Verify SHA-256 hash for this content. */
        sha256ContextGetHash(&(ctx->sha256_ctx), nca_ctx->hash);

        if (memcmp(nca_ctx->hash, nca_ctx->content_id.c, sizeof(nca_ctx->content_id.c)) != 0)
        {
            LOG_MSG_ERROR("SHA-256 checksum mismatch for %s NCA \"%s\"! (title %016lX).", titleGetNcmContentTypeName(nca_ctx->content_type), \
                                                                                          nca_ctx->content_id_str, nca_ctx->title_id);
            goto end;
        }

        /* Update system update context. */
        ctx->content_idx++;
        ctx->cur_content_offset = 0;
    }

    /* Update return value. */
    success = true;

end:
    return success;
}

static bool _systemUpdateInitializeDumpContext(SystemUpdateDumpContext *ctx)
{
    if (!g_systemUpdateInterfaceInit || !ctx)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return false;
    }

    bool success = false;

    /* Free output context beforehand. */
    systemUpdateFreeDumpContext(ctx);

    /* Loop through all of our content meta info records. */
    for(u16 i = 0; i < g_systemUpdateCnmtContext.packaged_header->content_meta_count; i++)
    {
        NcmContentMetaInfo *cur_meta_info = &(g_systemUpdateCnmtContext.content_meta_info[i]);

        /* Process current content meta info record. */
        if (!systemUpdateProcessContentMetaInfo(ctx, cur_meta_info))
        {
            LOG_MSG_ERROR("Failed to process content meta info for title %016lX!", cur_meta_info->id);
            goto end;
        }
    }

    /* Manually add SystemUpdate content records. */
    /* The SystemUpdate CNMT doesn't reference itself. */
    success = systemUpdateProcessContentRecords(ctx, g_systemUpdateTitleInfo);
    if (!success)
    {
        LOG_MSG_ERROR("Failed to process SystemUpdate content records!");
        goto end;
    }

    /* Sort NCA contexts. */
    qsort(ctx->nca_ctxs, ctx->content_count, sizeof(NcaContext*), &systemUpdateNcaContextSortFunction);

end:
    /* Free output context, if needed. */
    if (!success) systemUpdateFreeDumpContext(ctx);

    return success;
}

static bool systemUpdateProcessContentMetaInfo(SystemUpdateDumpContext *ctx, const NcmContentMetaInfo *content_meta_info)
{
    if (!ctx || !content_meta_info)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return false;
    }

    TitleInfo *title_info = NULL;
    bool success = false;

    /* Get TitleInfo entry. */
    title_info = titleGetTitleInfoEntryFromStorageByTitleId(NcmStorageId_BuiltInSystem, content_meta_info->id);
    if (!title_info)
    {
        LOG_MSG_ERROR("Failed to get TitleInfo entry for ID %016lX!", content_meta_info->id);
        goto end;
    }

    /* Check for matching version values. */
    if (title_info->version.value != content_meta_info->version)
    {
        LOG_MSG_ERROR("Version mismatch for title %016lX! (got v%u, expected v%u).", content_meta_info->id, title_info->version.value, \
                                                                                    content_meta_info->version);
        goto end;
    }

    /* Process content records. */
    success = systemUpdateProcessContentRecords(ctx, title_info);
    if (!success) LOG_MSG_ERROR("Failed to process content records for title %016lX!", content_meta_info->id);

end:
    titleFreeTitleInfo(&title_info);

    return success;
}

static bool systemUpdateProcessContentRecords(SystemUpdateDumpContext *ctx, TitleInfo *title_info)
{
    if (!ctx || !titleIsValidInfoBlock(title_info))
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return false;
    }

    NcaContext **nca_ctxs_tmp = NULL;
    const u32 nca_ctxs_count = (ctx->content_count + title_info->content_count);
    bool success = false;

    /* Reallocate NCA context pointer array. */
    nca_ctxs_tmp = realloc(ctx->nca_ctxs, nca_ctxs_count * sizeof(NcaContext*));
    if (!nca_ctxs_tmp)
    {
        LOG_MSG_ERROR("Failed to reallocate NCA context pointer array for title %016lX! (%u %s).", title_info->meta_key.id, nca_ctxs_count, \
                                                                                                   nca_ctxs_count > 1 ? "entries" : "entry");
        goto end;
    }

    memset(&(nca_ctxs_tmp[ctx->content_count]), 0, title_info->content_count * sizeof(NcaContext*));
    ctx->nca_ctxs = nca_ctxs_tmp;

    /* Loop through all of the content records for the current title. */
    for(u32 i = 0; i < title_info->content_count; i++)
    {
        NcmContentInfo *cur_content_info = &(title_info->content_infos[i]);
        NcaContext **cur_nca_ctx = &(ctx->nca_ctxs[ctx->content_count + i]);

        /* Allocate memory for the current NCA context. */
        *cur_nca_ctx = calloc(1, sizeof(NcaContext));
        if (!*cur_nca_ctx)
        {
            LOG_MSG_ERROR("Failed to allocate memory for NCA context! (title %016lX, content #%u).", title_info->meta_key.id, i);
            goto end;
        }

        /* Initialize current NCA context. */
        if (!ncaInitializeContext(*cur_nca_ctx, title_info->storage_id, 0, &(title_info->meta_key), cur_content_info, NULL) || !(*cur_nca_ctx)->valid_main_signature)
        {
            LOG_MSG_ERROR("Failed to initialize NCA context! (title %016lX, content #%u).", title_info->meta_key.id, i);
            goto end;
        }
    }

    /* Update context. */
    ctx->total_size += title_info->size;
    ctx->content_count += title_info->content_count;

    /* Update return value. */
    success = true;

end:
    /* Free previously allocated NCA contexts if we ran into a problem. */
    if (!success && nca_ctxs_tmp)
    {
        for(u32 i = 0; i < title_info->content_count; i++)
        {
            if (!ctx->nca_ctxs[ctx->content_count + i]) continue;
            free(ctx->nca_ctxs[ctx->content_count + i]);
            ctx->nca_ctxs[ctx->content_count + i] = NULL;
        }
    }

    return success;
}

static int systemUpdateNcaContextSortFunction(const void *a, const void *b)
{
    const NcaContext *nca_ctx_1 = *((const NcaContext**)a);
    const NcaContext *nca_ctx_2 = *((const NcaContext**)b);

    if (nca_ctx_1->title_id < nca_ctx_2->title_id)
    {
        return -1;
    } else
    if (nca_ctx_1->title_id > nca_ctx_2->title_id)
    {
        return 1;
    }

    if (nca_ctx_1->title_version.value < nca_ctx_2->title_version.value)
    {
        return -1;
    } else
    if (nca_ctx_1->title_version.value > nca_ctx_2->title_version.value)
    {
        return 1;
    }

    if (nca_ctx_1->content_type < nca_ctx_2->content_type)
    {
        return -1;
    } else
    if (nca_ctx_1->content_type > nca_ctx_2->content_type)
    {
        return 1;
    }

    if (nca_ctx_1->id_offset < nca_ctx_2->id_offset)
    {
        return -1;
    } else
    if (nca_ctx_1->id_offset > nca_ctx_2->id_offset)
    {
        return 1;
    }

    return 0;
}

NX_INLINE NcaContext *systemUpdateGetCurrentNcaContextFromDumpContext(SystemUpdateDumpContext *ctx)
{
    return ((systemUpdateIsValidDumpContext(ctx) && !systemUpdateIsDumpContextFinished(ctx)) ? ctx->nca_ctxs[ctx->content_idx] : NULL);
}
