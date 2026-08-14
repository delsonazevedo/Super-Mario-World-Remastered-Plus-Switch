/* hotfix_credits.h -- embedded fallback for res://credits.txt.
 *
 * The BTFighter/Boswer811/JoeMama lineage of this game ships a project-root
 * credits.txt (no Godot import metadata) that Android export presets have
 * historically dropped from the "non-resource files" export filter. Its only
 * reader, Scripts/Classes/Levels/staff_credits.gd, does:
 *
 *   credits = FileAccess.open("res://credits.txt", FileAccess.READ).get_as_text()
 *
 * with no null check -- a missing file crashes the whole process (Data Abort
 * at 0x0) the instant the post-Bowser fireworks scene finishes and the game
 * transitions to the credits scene. This is the ONLY unguarded FileAccess
 * open+chain in the entire codebase; confirmed missing from our extracted
 * APK assets; confirmed by a user-supplied Atmosphere crash report showing
 * exactly that null-deref signature.
 *
 * Content below is verbatim from the game's own open-source repository
 * (ISC), not authored by us -- we are only restoring what export dropped.
 * MIT license for this file; see LICENSE. */

#ifndef __HOTFIX_CREDITS_H__
#define __HOTFIX_CREDITS_H__

static const char HOTFIX_CREDITS_TXT[] =
  "--Super Mario World Remastered Plus\n"
  "\n"
  "--Created by\n"
  "JoeMama\n"
  "\n"
  "--Mod by\n"
  "Boswer\n"
  "\n"
  "--Sprites\n"
  "\n"
  "--Mario Luigi Toad Toadette Sprites\n"
  "GlacialSiren484\n"
  "MauricioN64\n"
  "AwesomeZack\n"
  "Jamestendo64\n"
  "LinkStormZ\n"
  "\n"
  "--Super Mario Advance 2 Hybrid Luigi Sprites\n"
  "Kaching720\n"
  "\n"
  "--Modern Peach Sprites\n"
  "GlacialSiren484\n"
  "\n"
  "--Modern Bowser Colours\n"
  "SilverBolt\n"
  "\n"
  "--Baby Yoshi Egg Sprites\n"
  "KGK64\n"
  "\n"
  "--Goomba and Goombrat Sprites\n"
  "SamButSam\n"
  "\n"
  "--Overhaul Autumn Colour Palette\n"
  "LinkStormZ\n"
  "\n"
  "--Super Mario World Sprite Rips\n"
  "Barack Obama TSR\n"
  "\n"
  "--Extended Switch Palace Tileset\n"
  "fireluigi\n"
  "\n"
  "--Test Level Tileset + Background\n"
  "SuperSledgeBro\n"
  "\n"
  "--Mario Construct Chain Chomp, Blooper, Angry Sun, Spike\n"
  "Smuglutena\n"
  "\n"
  "--Achievements\n"
  "RetroAchievements.com\n"
  "\n"
  "--Air Twirl, Propeller SFX\n"
  "LMPuny\n"
  "\n"
  "\n"
  "--Music (SMWC)\n"
  "\n"
  "--Overworld Theme Remix\n"
  "lu9\n"
  "\n"
  "--Overworld Autumn Alternative Remix (VLDC Beach)\n"
  "Vinci\n"
  "\n"
  "--Athletic Theme Remix\n"
  "HarvettFox96\n"
  "\n"
  "--Autumn Map Theme Music\n"
  "HarvettFox96\n"
  "\n"
  "--Autumn Yoshis Island Map Remix\n"
  "Jimmy \n"
  "\n"
  "--Autumn Vanilla Dome\n"
  "xyz600sp\n"
  "\n"
  "--Star Road, Illusion Forest, Underground Autumn Remixes\n"
  "MakerK6\n"
  "\n"
  "--Settings Menu\n"
  "Fyre150\n"
  "\n"
  "--SMB1 Map Theme\n"
  "MinecraftGamerLR\n"
  "Segment1Zone2\n"
  "\n"
  "\n"
  "--Special Thanks\n"
  "Dykas Kong\n"
  "wye\n"
  "neoarc\n"
  "hyron\n"
  "ItsVorzo\n"
  "MakerK6\n"
  "Przemo\n"
  "Cube\n"
  "SMMP Dev Team\n"
  "SMWR Beta Testers\n"
  "\n"
  "And You\n"
  "\n"
  "\n"
  "This game does not act as a substitute for the original Super Mario World.\n"
  "\n"
  "Super Mario World can be played now on Nintendo Switch Online.\n"
  "\n"
  "Please dont sue me Nintendo...\n"
  "\n"
  "Please....\n";

#endif
