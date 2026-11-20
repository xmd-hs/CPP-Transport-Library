#include <cldt_client_sdk.h>

/* This consumer compiles C, includes only the installed public header and
 * links only the exported binary target. No SDK source is available here. */
int main(void)
{
    cldt_client *client = cldt_client_create("127.0.0.1", 19408, "", 0, 0, 0);
    if (!client)
    {
        return 1;
    }
    cldt_client_options options;
    cldt_client_options_init(&options);
    if (options.download_durability != CLDT_DURABILITY_SYNC ||
        cldt_client_set_options(client, &options) != CLDT_STATUS_OK ||
        cldt_client_set_file_sync(client, 0) != CLDT_STATUS_OK ||
        cldt_client_set_file_io(client, 0) != CLDT_STATUS_OK ||
        cldt_client_cancel(client) != CLDT_STATUS_OK)
    {
        cldt_client_destroy(client);
        return 2;
    }
    cldt_client_destroy(client);
    return 0;
}
