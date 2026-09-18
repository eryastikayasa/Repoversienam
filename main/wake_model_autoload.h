#pragma once

// Auto-load ESP-SR models from the SPIFFS partition before any WakeNet
// handle lookup. This keeps the existing main.cpp WakeNet code unchanged.
#include "model_path.h"
#include "esp_wn_models.h"

static inline const esp_wn_iface_t *repo_original_esp_wn_handle_from_name(const char *model_name)
{
    using fn_t = const esp_wn_iface_t *(*)(const char *);
    static fn_t fn = &esp_wn_handle_from_name;
    return fn(model_name);
}

static inline const esp_wn_iface_t *repo_esp_wn_handle_from_name_autoload(const char *model_name)
{
    if (get_static_srmodels() == nullptr) {
        srmodel_list_t *models = esp_srmodel_init("model");
        if (models == nullptr || models->num <= 0) {
            return nullptr;
        }
    }
    return repo_original_esp_wn_handle_from_name(model_name);
}

#define esp_wn_handle_from_name repo_esp_wn_handle_from_name_autoload
