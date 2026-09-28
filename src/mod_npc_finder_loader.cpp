/*
 * mod-npc-finder loader.
 *
 * AzerothCore looks up a loader symbol derived from the module's folder name: for folder
 * "mod-npc-finder" that symbol is exactly "Addmod_npc_finderScripts". If you clone the repo under a
 * different folder name, rename this function to match.
 *
 * Released under the MIT License.
 */

void AddNpcFinderScripts();

void Addmod_npc_finderScripts()
{
    AddNpcFinderScripts();
}
