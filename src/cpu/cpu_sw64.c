/*
 * cpu_sw64.c: CPU driver for sw64 CPUs
 *
 * Copyright (C) 2021 Lu Feifei
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "virfile.h"
#include "viralloc.h"
#include "cpu.h"
#include "cpu_sw64.h"
#include "cpu_map.h"
#include "virstring.h"
#include "virhostcpu.h"

#define VIR_FROM_THIS VIR_FROM_CPU
#define ARRAY_CARDINALITY(Array) (sizeof(Array) / sizeof(*(Array)))
#define CPUINFO_PATH "/proc/cpuinfo"

static const virArch archs[] = { VIR_ARCH_SW_64 };

typedef struct _sw64Model sw64Model;
struct _sw64Model {
    char *name;
};

typedef struct _sw64Map sw64Map;
struct _sw64Map {
    size_t nmodels;
    sw64Model **models;
};

static virCPUCompareResult
virCPUsw64Compare(virCPUDef *host ATTRIBUTE_UNUSED,
                  virCPUDef *cpu ATTRIBUTE_UNUSED,
                  bool failMessages ATTRIBUTE_UNUSED)
{
    return VIR_CPU_COMPARE_IDENTICAL;
}

static int
virCPUsw64Update(virCPUDef *guest,
                 const virCPUDef *host,
                 bool relative ATTRIBUTE_UNUSED,
                 virCPUFeaturePolicy removedPolicy G_GNUC_UNUSED)
{
    g_autoptr(virCPUDef) updated = NULL;

    if (!relative || guest->mode != VIR_CPU_MODE_HOST_MODEL)
        return 0;

    if (!host) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("unknown host CPU model"));
        return -1;
    }

    if (!(updated = virCPUDefCopyWithoutModel(guest)))
        return -1;

    updated->mode = VIR_CPU_MODE_CUSTOM;
    virCPUDefCopyModel(updated, host, true);

    virCPUDefStealModel(guest, updated, false);
    guest->mode = VIR_CPU_MODE_CUSTOM;
    guest->match = VIR_CPU_MATCH_EXACT;

    return 0;
}

static void
sw64ModelFree(sw64Model *model)
{
    if (!model)
        return;

    VIR_FREE(model->name);
    VIR_FREE(model);
}

static void
sw64MapFree(sw64Map *map)
{
    size_t i;

    if (!map)
        return;

    for (i = 0; i < map->nmodels; i++)
        sw64ModelFree(map->models[i]);
    VIR_FREE(map->models);
    VIR_FREE(map);
}

static sw64Model *
sw64ModelFind(const sw64Map *map,
              const char *name)
{
    size_t i;

    for (i = 0; i < map->nmodels; i++) {
        if (STREQ(map->models[i]->name, name))
            return map->models[i];
    }

    return NULL;
}

static int
sw64ModelParse(xmlXPathContextPtr ctxt ATTRIBUTE_UNUSED,
               const char *name,
               void *data)
{
    sw64Map *map = data;
    sw64Model *model;
    int ret = -1;

    model = g_new0(sw64Model, 1);

    model->name = g_strdup(name);

    if (sw64ModelFind(map, model->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU model %1$s already defined"), model->name);
        goto cleanup;
    }

    VIR_APPEND_ELEMENT(map->models, map->nmodels, model);

    ret = 0;

 cleanup:
    sw64ModelFree(model);
    return ret;
}

static sw64Map *
sw64LoadMap(void)
{
    sw64Map *map;

    map = g_new0(sw64Map, 1);

    if (cpuMapLoad("sw64", NULL, NULL, sw64ModelParse, map) < 0)
        goto error;

    return map;

 error:
    sw64MapFree(map);
    return NULL;
}

static int
sw64CPUParseCpuModeString(const char *str,
		          const char *prefix,
			  unsigned int *mode)
{
    char *p;
    unsigned int ui;
    /* If the string doesn't start with the expected prefix, then
     * we're not looking at the right string and we should move on */
    if (!STRPREFIX(str, prefix))
        return 1;
    /* Skip the prefix */
    str += strlen(prefix);

    /* Skip all whitespace */
    while (g_ascii_isspace(*str))
        str++;
    if (*str == '\0')
        goto error;

    /* Skip the colon. If anything but a colon is found, then we're
     * not looking at the right string and we should move on */
    if (*str != ':')
        return 1;
    str++;

    /* Skip all whitespace */
    while (g_ascii_isspace(*str))
        str++;
    if (*str == '\0')
        goto error;

    if (virStrToLong_ui(str, &p, 10, &ui) < 0 ||
        (*p != '.' && *p != '\0' && !g_ascii_isspace(*p))) {
        goto error;
    }

    *mode = ui;
    return 0;

 error:
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Missing or invalid CPU variation in %s"),
                   CPUINFO_PATH);
    return -1;
}

static int
sw64CPUParseCpuMode(FILE *cpuinfo, unsigned int *mode)
{
    const char *prefix = "cpu variation";
    char line[1024];

    while (fgets(line, sizeof(line), cpuinfo) != NULL) {
        if (sw64CPUParseCpuModeString(line, prefix, mode) < 0)
            return -1;
    }

    return 0;
}

static int
virCPUsw64GetHost(virCPUDef *cpu,
                  virDomainCapsCPUModels *models ATTRIBUTE_UNUSED)
{
    int ret = -1;
    unsigned int mode;
    FILE *cpuinfo = fopen(CPUINFO_PATH, "r");
    if (!cpuinfo) {
        virReportSystemError(errno,
                             _("cannot open %s"), CPUINFO_PATH);
	return -1;
    }

    ret = sw64CPUParseCpuMode(cpuinfo, &mode);
    if (ret < 0)
        goto cleanup;

    if (mode == 3)
        cpu->model = g_strdup("core3");
    else if (mode == 4)
        cpu->model = g_strdup("core4");

 cleanup:
    VIR_FORCE_FCLOSE(cpuinfo);
    return ret;
}

static int
virCPUsw64DriverGetModels(char ***models)
{
    sw64Map *map;
    size_t i;
    int ret = -1;

    if (!(map = sw64LoadMap()))
        goto error;

    if (models) {
        *models = g_new0(char *, map->nmodels + 1);

        for (i = 0; i < map->nmodels; i++) {
            (*models)[i] = g_strdup(map->models[i]->name);
        }
    }

    ret = map->nmodels;

 cleanup:
    sw64MapFree(map);
    return ret;

 error:
    if (models) {
        g_strfreev(*models);
        *models = NULL;
    }
    goto cleanup;
}

struct cpuArchDriver cpuDriverSW64 = {
    .name = "sw_64",
    .arch = archs,
    .narch = G_N_ELEMENTS(archs),
    .getHost = virCPUsw64GetHost,
    .compare = virCPUsw64Compare,
    .decode = NULL,
    .encode = NULL,
    .baseline = NULL,
    .update = virCPUsw64Update,
    .getModels = virCPUsw64DriverGetModels,
};
