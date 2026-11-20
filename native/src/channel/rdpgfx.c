#include "wsland/server.h"


static UINT rdpgfx_client_caps_advertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise) {
    struct wsland_client *client = context->custom;
    RdpgfxServerContext *gfx_ctx = client->rdpgfx_ctx;

    for (int i = 0; i < capsAdvertise->capsSetCount; i++) {
        RDPGFX_CAPSET *capsSet = &capsAdvertise->capsSets[i];

        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8: // NOLINT(*-branch-clone)
            break;
        case RDPGFX_CAPVERSION_81:
            break;
        case RDPGFX_CAPVERSION_10:
            break;
        case RDPGFX_CAPVERSION_101:
            break;
        case RDPGFX_CAPVERSION_102:
            break;
        case RDPGFX_CAPVERSION_103:
            break;
        case RDPGFX_CAPVERSION_104:
            break;
        case RDPGFX_CAPVERSION_105:
            break;
        case RDPGFX_CAPVERSION_106:
            break;
        default: ;
        }

        if (capsSet->flags & RDPGFX_CAPS_FLAG_THINCLIENT) {
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) {
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) {
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_THINCLIENT) {
        }

        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8: // NOLINT(*-branch-clone)
            {
                /*RDPGFX_CAPSET_VERSION8 *caps8 = (RDPGFX_CAPSET_VERSION8 *)capsSet;*/
                break;
            }
        case RDPGFX_CAPVERSION_81:
            {
                /*RDPGFX_CAPSET_VERSION81 *caps81 = (RDPGFX_CAPSET_VERSION81 *)capsSet;*/
                break;
            }
        case RDPGFX_CAPVERSION_10:
        case RDPGFX_CAPVERSION_101:
        case RDPGFX_CAPVERSION_102:
        case RDPGFX_CAPVERSION_103:
        case RDPGFX_CAPVERSION_104:
        case RDPGFX_CAPVERSION_105:
        case RDPGFX_CAPVERSION_106:
            {
                /*RDPGFX_CAPSET_VERSION10 *caps10 = (RDPGFX_CAPSET_VERSION10 *)capsSet;*/
                break;
            }
        default: ;
        }
    }

    RDPGFX_CAPS_CONFIRM_PDU capsConfirm = {0};

    capsConfirm.capsSet = capsAdvertise->capsSets; /* TODO: choose right one.*/
    gfx_ctx->CapsConfirm(gfx_ctx, &capsConfirm);

    client->activation_graphics_completed = true;
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_cache_import_offer(RdpgfxServerContext *context, const RDPGFX_CACHE_IMPORT_OFFER_PDU *_) {
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_frame_acknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge) {
    struct wsland_client *client = context->custom;

    client->acknowledged_fid = frameAcknowledge->frameId;
    return CHANNEL_RC_OK;
}

bool server_client_rdpgfx_init(struct wsland_client *client) {
    RdpgfxServerContext *gfx_ctx = rdpgfx_server_context_new(client->vcm);
    if (!gfx_ctx) {
        return false;
    }
    client->rdpgfx_ctx = gfx_ctx;

    gfx_ctx->custom = client;
    gfx_ctx->CapsAdvertise = rdpgfx_client_caps_advertise;
    gfx_ctx->CacheImportOffer = rdpgfx_client_cache_import_offer;
    gfx_ctx->FrameAcknowledge = rdpgfx_client_frame_acknowledge;
    if (!gfx_ctx->Open(gfx_ctx)) {
        return false;
    }

    return true;
}