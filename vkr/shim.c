/*
 * Stand-ins for the parts of libvirgl that vkr links against but that we
 * don't build (the virgl resource table belongs to the GL renderer).
 * SPDX-License-Identifier: MIT
 */
#include "virgl_resource.h"

/* only reached when importing an opaque-fd resource, which we never do */
enum virgl_resource_fd_type
virgl_resource_export_fd(struct virgl_resource *res, int *fd)
{
   (void)res;
   *fd = -1;
   return VIRGL_RESOURCE_FD_INVALID;
}
