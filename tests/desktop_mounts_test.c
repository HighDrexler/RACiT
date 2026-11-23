#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern int ps3_vfs_override_path(const char* guest, char* host, size_t capacity);

int main(void)
{
    char path[128];
    assert(unsetenv("PS3_HDD0_HOME_ROOT") == 0);
    assert(unsetenv("PS3_UPDATE_PARAM_SFO") == 0);
    assert(ps3_vfs_override_path("/dev_hdd0/home", path, sizeof path) == 0);
    assert(ps3_vfs_override_path("/dev_bdvd/PS3_GAME/PARAM.SFO", path, sizeof path) == 0);

    assert(setenv("PS3_HDD0_HOME_ROOT", "/profiles/disc home", 1) == 0);
    assert(setenv("PS3_UPDATE_PARAM_SFO", "/profiles/update/PARAM.SFO", 1) == 0);
    assert(ps3_vfs_override_path("/dev_hdd0/home", path, sizeof path) == 1);
    assert(strcmp(path, "/profiles/disc home") == 0);
    assert(ps3_vfs_override_path("/dev_hdd0/home/00000001/savedata", path, sizeof path) == 1);
    assert(strcmp(path, "/profiles/disc home/00000001/savedata") == 0);
    assert(ps3_vfs_override_path("/dev_hdd0/homeland", path, sizeof path) == 0);
    assert(ps3_vfs_override_path("/dev_hdd0/game/BCUS98124/USRDIR", path, sizeof path) == 0);
    assert(ps3_vfs_override_path("/dev_bdvd/PS3_GAME/PARAM.SFO", path, sizeof path) == 1);
    assert(strcmp(path, "/profiles/update/PARAM.SFO") == 0);
    assert(ps3_vfs_override_path("/dev_bdvd/PS3_GAME/USRDIR/PARAM.SFO", path, sizeof path) == 0);
    assert(ps3_vfs_override_path("/dev_bdvd/PS3_GAME/PARAM.SFO", path, 5) == -1);
}
