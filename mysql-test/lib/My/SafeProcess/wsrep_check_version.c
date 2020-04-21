/*
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */

#include <my_global.h>
#include <string.h>
#include "../../../../wsrep-lib/wsrep-API/v26/wsrep_api.h"

int main(int argc, char **argv)
{
  int rc= 1;
  void *dlh;
  const char *provider= getenv("WSREP_PROVIDER");
  char** dlversion= NULL;

  if (!provider || !*provider)
  {
    printf("WSREP_PROVIDER is not set\n");
    return 1;
  }
  if (!(dlh= dlopen(provider, RTLD_NOW | RTLD_LOCAL)))
  {
    printf("Can't open WSREP_PROVIDER (%s) library, error: %s\n",
           provider, dlerror());
    return 1;
  }

  dlversion= (char**) dlsym(dlh, "wsrep_interface_version");
  if (dlversion && *dlversion)
  {
    rc= strcmp(*dlversion, WSREP_INTERFACE_VERSION) ? 2 : 0;
    if (rc)
      printf("Wrong wsrep provider library version, found: %s, need: %s\n", *dlversion, WSREP_INTERFACE_VERSION);
  }
  else
    printf("Galera library does not contain a version symbol");

  dlclose(dlh);
  return rc;
}
