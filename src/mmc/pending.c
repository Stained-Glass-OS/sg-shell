/* placeholders for the consoles still being written */
#include "mmc.h"
node_t *users_create(node_t *parent) { return node_add(parent, L"Local Users and Groups", IC_USERS, NULL, NULL); }
node_t *shares_create(node_t *parent) { return node_add(parent, L"Shared Folders", IC_SHARE, NULL, NULL); }
