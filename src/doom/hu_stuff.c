//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:  Heads-up displays
//


#include <ctype.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomkeys.h"

#include "z_zone.h"

#include "deh_main.h"
#include "i_input.h"
#include "i_swap.h"
#include "i_video.h"

#include "hu_stuff.h"
#include "hu_lib.h"
#include "m_controls.h"
#include "m_misc.h"
#include "w_wad.h"

#include "s_sound.h"

#include "doomstat.h"

#include "r_state.h"
#include "r_main.h"
#include "m_fixed.h"
#include "v_video.h"
#include "d_loop.h"
#include "d_player.h"
#include "net_client.h"

// Data.
#include "dstrings.h"
#include "sounds.h"

//
// Locally used constants, shortcuts.
//
#define HU_TITLE	(mapnames[(gameepisode-1)*9+gamemap-1])
#define HU_TITLE2	(mapnames_commercial[gamemap-1])
#define HU_TITLEP	(mapnames_commercial[gamemap-1 + 32])
#define HU_TITLET	(mapnames_commercial[gamemap-1 + 64])
#define HU_TITLE_CHEX   (mapnames_chex[(gameepisode-1)*9+gamemap-1])
#define HU_TITLEHEIGHT	1
#define HU_TITLEX	0
#define HU_TITLEY	(167 - SHORT(hu_font[0]->height))

#define HU_INPUTTOGGLE	't'
#define HU_INPUTX	HU_MSGX
#define HU_INPUTY	(HU_MSGY + HU_MSGHEIGHT*(SHORT(hu_font[0]->height) +1))
#define HU_INPUTWIDTH	64
#define HU_INPUTHEIGHT	1



char *chat_macros[10];

const char *player_names[] =
{
    HUSTR_PLRGREEN,
    HUSTR_PLRINDIGO,
    HUSTR_PLRBROWN,
    HUSTR_PLRRED
};

// Network player names: filled by name exchange packets.
// Defaults to color names until real names arrive.
// Initialized statically so D_CheckNetGame() name exchange isn't wiped by HU_Init().
char net_player_names[4][HU_MAXPLAYERNAME] = {
    "Green", "Indigo", "Brown", "Red"
};
int player_pings[4];
static char net_player_prefixes[4][HU_MAXPLAYERNAME + 4] = {
    "Green: ", "Indigo: ", "Brown: ", "Red: "
};

char			chat_char; // remove later.
static player_t*	plr;
patch_t*		hu_font[HU_FONTSIZE];
static hu_textline_t	w_title;
boolean			chat_on;
static hu_itext_t	w_chat;
static boolean		always_off = false;
static char		chat_dest[MAXPLAYERS];
static hu_itext_t w_inputbuffer[MAXPLAYERS];

static boolean		message_on;
boolean			message_dontfuckwithme;
static boolean		message_nottobefuckedwith;

static hu_stext_t	w_message;
static int		message_counter;

extern int		showMessages;

static boolean		headsupactive = false;

static const char *player_colors[] = { "Green", "Indigo", "Brown", "Red" };

void HU_SetPlayerName(int player, const char *name) {
    if (player < 0 || player >= 4) return;
    M_StringCopy(net_player_names[player], name, HU_MAXPLAYERNAME);
    // Chat prefix includes color for identification: "Name (Color): "
    M_snprintf(net_player_prefixes[player], sizeof(net_player_prefixes[player]),
               "%s (%s): ", name, player_colors[player]);
    player_names[player] = net_player_prefixes[player];
}

void HU_DisplayNetMessage(int player, const char *msg) {
    if (player < 0 || player >= 4) return;
    HUlib_addMessageToSText(&w_message,
                            DEH_String(player_names[player]),
                            msg);
    message_nottobefuckedwith = true;
    message_on = true;
    message_counter = HU_MSGTIMEOUT;
    if (gamemode == commercial)
        S_StartSound(0, sfx_radio);
    else
        S_StartSound(0, sfx_tink);
}

void HU_DisplayKillMessage(const char *msg) {
    HUlib_addMessageToSText(&w_message, 0, msg);
    message_nottobefuckedwith = true;
    message_on = true;
    message_counter = HU_MSGTIMEOUT;
}

//
// Builtin map names.
// The actual names can be found in DStrings.h.
//

const char *mapnames[] =	// DOOM shareware/registered/retail (Ultimate) names.
{

    HUSTR_E1M1,
    HUSTR_E1M2,
    HUSTR_E1M3,
    HUSTR_E1M4,
    HUSTR_E1M5,
    HUSTR_E1M6,
    HUSTR_E1M7,
    HUSTR_E1M8,
    HUSTR_E1M9,

    HUSTR_E2M1,
    HUSTR_E2M2,
    HUSTR_E2M3,
    HUSTR_E2M4,
    HUSTR_E2M5,
    HUSTR_E2M6,
    HUSTR_E2M7,
    HUSTR_E2M8,
    HUSTR_E2M9,

    HUSTR_E3M1,
    HUSTR_E3M2,
    HUSTR_E3M3,
    HUSTR_E3M4,
    HUSTR_E3M5,
    HUSTR_E3M6,
    HUSTR_E3M7,
    HUSTR_E3M8,
    HUSTR_E3M9,

    HUSTR_E4M1,
    HUSTR_E4M2,
    HUSTR_E4M3,
    HUSTR_E4M4,
    HUSTR_E4M5,
    HUSTR_E4M6,
    HUSTR_E4M7,
    HUSTR_E4M8,
    HUSTR_E4M9,

    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL"
};

const char *mapnames_chex[] =   // Chex Quest names.
{

    HUSTR_E1M1,
    HUSTR_E1M2,
    HUSTR_E1M3,
    HUSTR_E1M4,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,

    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,

    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,

    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,
    HUSTR_E1M5,

    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL",
    "NEWLEVEL"
};

// List of names for levels in commercial IWADs
// (doom2.wad, plutonia.wad, tnt.wad).  These are stored in a
// single large array; WADs like pl2.wad have a MAP33, and rely on
// the layout in the Vanilla executable, where it is possible to
// overflow the end of one array into the next.

const char *mapnames_commercial[] =
{
    // DOOM 2 map names.

    HUSTR_1,
    HUSTR_2,
    HUSTR_3,
    HUSTR_4,
    HUSTR_5,
    HUSTR_6,
    HUSTR_7,
    HUSTR_8,
    HUSTR_9,
    HUSTR_10,
    HUSTR_11,
	
    HUSTR_12,
    HUSTR_13,
    HUSTR_14,
    HUSTR_15,
    HUSTR_16,
    HUSTR_17,
    HUSTR_18,
    HUSTR_19,
    HUSTR_20,
	
    HUSTR_21,
    HUSTR_22,
    HUSTR_23,
    HUSTR_24,
    HUSTR_25,
    HUSTR_26,
    HUSTR_27,
    HUSTR_28,
    HUSTR_29,
    HUSTR_30,
    HUSTR_31,
    HUSTR_32,

    // Plutonia WAD map names.

    PHUSTR_1,
    PHUSTR_2,
    PHUSTR_3,
    PHUSTR_4,
    PHUSTR_5,
    PHUSTR_6,
    PHUSTR_7,
    PHUSTR_8,
    PHUSTR_9,
    PHUSTR_10,
    PHUSTR_11,
	
    PHUSTR_12,
    PHUSTR_13,
    PHUSTR_14,
    PHUSTR_15,
    PHUSTR_16,
    PHUSTR_17,
    PHUSTR_18,
    PHUSTR_19,
    PHUSTR_20,
	
    PHUSTR_21,
    PHUSTR_22,
    PHUSTR_23,
    PHUSTR_24,
    PHUSTR_25,
    PHUSTR_26,
    PHUSTR_27,
    PHUSTR_28,
    PHUSTR_29,
    PHUSTR_30,
    PHUSTR_31,
    PHUSTR_32,
    
    // TNT WAD map names.

    THUSTR_1,
    THUSTR_2,
    THUSTR_3,
    THUSTR_4,
    THUSTR_5,
    THUSTR_6,
    THUSTR_7,
    THUSTR_8,
    THUSTR_9,
    THUSTR_10,
    THUSTR_11,
	
    THUSTR_12,
    THUSTR_13,
    THUSTR_14,
    THUSTR_15,
    THUSTR_16,
    THUSTR_17,
    THUSTR_18,
    THUSTR_19,
    THUSTR_20,
	
    THUSTR_21,
    THUSTR_22,
    THUSTR_23,
    THUSTR_24,
    THUSTR_25,
    THUSTR_26,
    THUSTR_27,
    THUSTR_28,
    THUSTR_29,
    THUSTR_30,
    THUSTR_31,
    THUSTR_32,

    // Emulation: TNT maps 33-35 can be warped to and played if they exist
    // so include blank names instead of spilling over
    "",
    "",
    ""
};

void HU_Init(void)
{

    int		i;
    int		j;
    char	buffer[9];

    // load the heads-up font
    j = HU_FONTSTART;
    for (i=0;i<HU_FONTSIZE;i++)
    {
	DEH_snprintf(buffer, 9, "STCFN%.3d", j++);
	hu_font[i] = (patch_t *) W_CacheLumpName(buffer, PU_STATIC);
    }

}

void HU_Stop(void)
{
    headsupactive = false;
}

void HU_Start(void)
{

    int		i;
    const char *s;

    if (headsupactive)
	HU_Stop();

    plr = &players[consoleplayer];
    message_on = false;
    message_dontfuckwithme = false;
    message_nottobefuckedwith = false;
    chat_on = false;

    // create the message widget
    HUlib_initSText(&w_message,
		    HU_MSGX, HU_MSGY, HU_MSGHEIGHT,
		    hu_font,
		    HU_FONTSTART, &message_on);

    // create the map title widget
    HUlib_initTextLine(&w_title,
		       HU_TITLEX, HU_TITLEY,
		       hu_font,
		       HU_FONTSTART);
    
    switch ( logical_gamemission )
    {
      case doom:
	s = HU_TITLE;
	break;
      case doom2:
	 s = HU_TITLE2;
         // Pre-Final Doom compatibility: map33-map35 names don't spill over
         if (gameversion <= exe_doom_1_9 && gamemap >= 33)
         {
             s = "";
         }
	 break;
      case pack_plut:
	s = HU_TITLEP;
	break;
      case pack_tnt:
	s = HU_TITLET;
	break;
      default:
         s = "Unknown level";
         break;
    }

    if (logical_gamemission == doom && gameversion == exe_chex)
    {
        s = HU_TITLE_CHEX;
    }

    // dehacked substitution to get modified level name

    s = DEH_String(s);
    
    while (*s)
	HUlib_addCharToTextLine(&w_title, *(s++));

    // create the chat widget
    HUlib_initIText(&w_chat,
		    HU_INPUTX, HU_INPUTY,
		    hu_font,
		    HU_FONTSTART, &chat_on);

    // create the inputbuffer widgets
    for (i=0 ; i<MAXPLAYERS ; i++)
	HUlib_initIText(&w_inputbuffer[i], 0, 0, 0, 0, &always_off);

    headsupactive = true;

}

#define NAMETAG_MINZ    (FRACUNIT*4)

void HU_DrawPlayerNames(void)
{
    int i;

    if (!netgame || deathmatch) return;

    for (i = 0; i < MAXPLAYERS; i++)
    {
        fixed_t tr_x, tr_y, gxt, gyt, tz, tx;
        fixed_t xscale, head_z, texturemid;
        int screen_x, screen_y;
        const char *name;
        int name_width, j;
        mobj_t *mo;
        int c;

        if (!playeringame[i] || i == consoleplayer) continue;
        mo = players[i].mo;
        if (!mo) continue;
        if (players[i].playerstate != PST_LIVE) continue;

        // Project player head position to screen
        tr_x = mo->x - viewx;
        tr_y = mo->y - viewy;

        gxt = FixedMul(tr_x, viewcos);
        gyt = -FixedMul(tr_y, viewsin);
        tz = gxt - gyt;

        if (tz < NAMETAG_MINZ) continue;

        xscale = FixedDiv(projection, tz);

        gxt = -FixedMul(tr_x, viewsin);
        gyt = FixedMul(tr_y, viewcos);
        tx = -(gyt + gxt);

        // Too far off the side
        if (abs(tx) > (tz << 2)) continue;

        screen_x = (centerxfrac + FixedMul(tx, xscale)) >> FRACBITS;

        // Y position: top of player sprite + offset above head
        head_z = mo->z + mo->height + (12 << FRACBITS);
        texturemid = head_z - viewz;
        screen_y = (centeryfrac - FixedMul(texturemid, xscale)) >> FRACBITS;

        // Bounds check: ensure font fits vertically
        {
            int font_h = SHORT(hu_font[0]->height);
            if (screen_y < 0 || screen_y + font_h > SCREENHEIGHT) continue;
        }

        // Get name and calculate pixel width (static size)
        name = net_player_names[i];
        name_width = 0;
        for (j = 0; name[j]; j++)
        {
            c = toupper((unsigned char)name[j]);
            if (c >= HU_FONTSTART && c <= HU_FONTEND)
                name_width += SHORT(hu_font[c - HU_FONTSTART]->width);
            else
                name_width += 4;
        }

        // Center horizontally
        screen_x -= name_width / 2;

        // Skip if entire name is off-screen
        if (screen_x + name_width <= 0 || screen_x >= SCREENWIDTH) continue;

        // Draw each character at native size, skipping out-of-bounds chars
        for (j = 0; name[j]; j++)
        {
            c = toupper((unsigned char)name[j]);
            if (c >= HU_FONTSTART && c <= HU_FONTEND)
            {
                patch_t *p = hu_font[c - HU_FONTSTART];
                int pw = SHORT(p->width);
                if (screen_x >= 0 && screen_x + pw <= SCREENWIDTH)
                    V_DrawPatch(screen_x, screen_y, p);
                screen_x += pw;
            }
            else
            {
                screen_x += 4;
            }
        }
    }
}

// Draw a string at (x, y) using HUD font. Returns the x position after drawing.
static int HU_DrawString(int x, int y, const char *str)
{
    int c;
    for (; *str; str++)
    {
        c = toupper((unsigned char)*str);
        if (c >= HU_FONTSTART && c <= HU_FONTEND)
        {
            patch_t *p = hu_font[c - HU_FONTSTART];
            int pw = SHORT(p->width);
            if (x >= 0 && x + pw <= SCREENWIDTH
                && y >= 0 && y + SHORT(p->height) <= SCREENHEIGHT)
                V_DrawPatch(x, y, p);
            x += pw;
        }
        else
        {
            x += 4;
        }
    }
    return x;
}

// Draw a right-aligned string ending at x_right.
static void HU_DrawStringRight(int x_right, int y, const char *str)
{
    int width = 0, c;
    const char *s;
    for (s = str; *s; s++)
    {
        c = toupper((unsigned char)*s);
        if (c >= HU_FONTSTART && c <= HU_FONTEND)
            width += SHORT(hu_font[c - HU_FONTSTART]->width);
        else
            width += 4;
    }
    HU_DrawString(x_right - width, y, str);
}

#define SB_X       4
#define SB_Y       4
#define SB_PAD     2
#define SB_COL_PING  160
#define SB_COL_SCORE 210
#define SB_WIDTH   240
#define SB_BG_COLOR 0  // palette index for black

void HU_DrawScoreboard(void)
{
    int i, y, row_h, num_rows, total_h;
    char buf[64];

    if (!automapactive || !netgame) return;

    row_h = SHORT(hu_font[0]->height) + SB_PAD;

    // Count active players for box sizing
    num_rows = 1; // header
    for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i]) num_rows++;

    total_h = num_rows * row_h + SB_PAD * 2;

    // Draw dark background
    V_DrawFilledBox(SB_X, SB_Y, SB_WIDTH, total_h, SB_BG_COLOR);

    y = SB_Y + SB_PAD;

    // Header
    HU_DrawString(SB_X + SB_PAD, y, "PLAYER");
    HU_DrawStringRight(SB_X + SB_COL_PING, y, "PING");
    HU_DrawStringRight(SB_X + SB_COL_SCORE, y,
                       deathmatch ? "FRAGS" : "KILLS");
    y += row_h;

    // Player rows
    for (i = 0; i < MAXPLAYERS; i++)
    {
        int ping, score, j;

        if (!playeringame[i]) continue;

        // Name
        if (i == consoleplayer)
        {
            M_snprintf(buf, sizeof(buf), "%s (YOU)", net_player_names[i]);
            HU_DrawString(SB_X + SB_PAD, y, buf);
        }
        else
        {
            HU_DrawString(SB_X + SB_PAD, y, net_player_names[i]);
        }

        // Ping (local player uses fresh value)
        ping = (i == consoleplayer) ? NET_CL_GetLatency() : player_pings[i];
        M_snprintf(buf, sizeof(buf), "%dMS", ping);
        HU_DrawStringRight(SB_X + SB_COL_PING, y, buf);

        // Score
        if (deathmatch)
        {
            score = 0;
            for (j = 0; j < MAXPLAYERS; j++)
                if (j != i) score += players[i].frags[j];
        }
        else
        {
            score = players[i].killcount;
        }
        M_snprintf(buf, sizeof(buf), "%d", score);
        HU_DrawStringRight(SB_X + SB_COL_SCORE, y, buf);

        y += row_h;
    }
}

void HU_Drawer(void)
{

    HUlib_drawSText(&w_message);
    HUlib_drawIText(&w_chat);
    if (automapactive)
	HUlib_drawTextLine(&w_title, false);
    HU_DrawPlayerNames();
    HU_DrawScoreboard();

}

void HU_Erase(void)
{

    HUlib_eraseSText(&w_message);
    HUlib_eraseIText(&w_chat);
    HUlib_eraseTextLine(&w_title);

}

void HU_Ticker(void)
{

    int i, rc;
    char c;

    // tick down message counter if message is up
    if (message_counter && !--message_counter)
    {
	message_on = false;
	message_nottobefuckedwith = false;
    }

    if (showMessages || message_dontfuckwithme)
    {

	// display message if necessary
	if ((plr->message && !message_nottobefuckedwith)
	    || (plr->message && message_dontfuckwithme))
	{
	    HUlib_addMessageToSText(&w_message, 0, plr->message);
	    plr->message = 0;
	    message_on = true;
	    message_counter = HU_MSGTIMEOUT;
	    message_nottobefuckedwith = message_dontfuckwithme;
	    message_dontfuckwithme = 0;
	}

    } // else message_on = false;

    // check for incoming chat characters
    if (netgame)
    {
	for (i=0 ; i<MAXPLAYERS; i++)
	{
	    if (!playeringame[i])
		continue;
	    if (i != consoleplayer
		&& (c = players[i].cmd.chatchar))
	    {
		if (c <= HU_BROADCAST)
		    chat_dest[i] = c;
		else
		{
		    rc = HUlib_keyInIText(&w_inputbuffer[i], c);
		    if (rc && c == KEY_ENTER)
		    {
			if (w_inputbuffer[i].l.len
			    && (chat_dest[i] == consoleplayer+1
				|| chat_dest[i] == HU_BROADCAST))
			{
			    HUlib_addMessageToSText(&w_message,
						    DEH_String(player_names[i]),
						    w_inputbuffer[i].l.l);
			    
			    message_nottobefuckedwith = true;
			    message_on = true;
			    message_counter = HU_MSGTIMEOUT;
			    if ( gamemode == commercial )
			      S_StartSound(0, sfx_radio);
			    else if (gameversion > exe_doom_1_2)
			      S_StartSound(0, sfx_tink);
			}
			HUlib_resetIText(&w_inputbuffer[i]);
		    }
		}
		players[i].cmd.chatchar = 0;
	    }
	}
    }

}

#define QUEUESIZE		128

static char	chatchars[QUEUESIZE];
static int	head = 0;
static int	tail = 0;


void HU_queueChatChar(char c)
{
    if (((head + 1) & (QUEUESIZE-1)) == tail)
    {
	plr->message = DEH_String(HUSTR_MSGU);
    }
    else
    {
	chatchars[head] = c;
	head = (head + 1) & (QUEUESIZE-1);
    }
}

char HU_dequeueChatChar(void)
{
    char c;

    if (head != tail)
    {
	c = chatchars[tail];
	tail = (tail + 1) & (QUEUESIZE-1);
    }
    else
    {
	c = 0;
    }

    return c;
}

static void StartChatInput(int dest)
{
    chat_on = true;
    HUlib_resetIText(&w_chat);
    HU_queueChatChar(HU_BROADCAST);

    I_StartTextInput(0, 8, SCREENWIDTH, 16);
}

static void StopChatInput(void)
{
    chat_on = false;
    I_StopTextInput();
}

boolean HU_Responder(event_t *ev)
{

    static char		lastmessage[HU_MAXLINELENGTH+1];
    const char		*macromessage;
    boolean		eatkey = false;
    static boolean	altdown = false;
    unsigned char 	c;
    int			i;
    int			numplayers;
    
    static int		num_nobrainers = 0;

    numplayers = 0;
    for (i=0 ; i<MAXPLAYERS ; i++)
	numplayers += playeringame[i];

    if (ev->data1 == KEY_RSHIFT)
    {
	return false;
    }
    else if (ev->data1 == KEY_RALT || ev->data1 == KEY_LALT)
    {
	altdown = ev->type == ev_keydown;
	return false;
    }

    if (ev->type != ev_keydown)
	return false;

    if (!chat_on)
    {
	if (ev->data1 == key_message_refresh)
	{
	    message_on = true;
	    message_counter = HU_MSGTIMEOUT;
	    eatkey = true;
	}
	else if (netgame && ev->data2 == key_multi_msg)
	{
	    eatkey = true;
            StartChatInput(HU_BROADCAST);
	}
	else if (netgame && numplayers > 2)
	{
	    for (i=0; i<MAXPLAYERS ; i++)
	    {
		if (ev->data2 == key_multi_msgplayer[i])
		{
		    if (playeringame[i] && i!=consoleplayer)
		    {
			eatkey = true;
                        StartChatInput(i + 1);
			break;
		    }
		    else if (i == consoleplayer)
		    {
			num_nobrainers++;
			if (num_nobrainers < 3)
			    plr->message = DEH_String(HUSTR_TALKTOSELF1);
			else if (num_nobrainers < 6)
			    plr->message = DEH_String(HUSTR_TALKTOSELF2);
			else if (num_nobrainers < 9)
			    plr->message = DEH_String(HUSTR_TALKTOSELF3);
			else if (num_nobrainers < 32)
			    plr->message = DEH_String(HUSTR_TALKTOSELF4);
			else
			    plr->message = DEH_String(HUSTR_TALKTOSELF5);
		    }
		}
	    }
	}
    }
    else
    {
	// send a macro
	if (altdown)
	{
	    c = ev->data1 - '0';
	    if (c > 9)
		return false;
	    // fprintf(stderr, "got here\n");
	    macromessage = chat_macros[c];

	    // kill last message with a '\n'
	    HU_queueChatChar(KEY_ENTER); // DEBUG!!!

	    // send the macro message
	    while (*macromessage)
		HU_queueChatChar(*macromessage++);
	    HU_queueChatChar(KEY_ENTER);

            // leave chat mode and notify that it was sent
            StopChatInput();
            M_StringCopy(lastmessage, chat_macros[c], sizeof(lastmessage));
            plr->message = lastmessage;
            eatkey = true;
	}
	else
	{
            c = ev->data3;

	    eatkey = HUlib_keyInIText(&w_chat, c);
	    if (eatkey)
	    {
		// static unsigned char buf[20]; // DEBUG
		HU_queueChatChar(c);

		// M_snprintf(buf, sizeof(buf), "KEY: %d => %d", ev->data1, c);
		//        plr->message = buf;
	    }
	    if (c == KEY_ENTER)
	    {
		StopChatInput();
                if (w_chat.l.len)
                {
                    M_StringCopy(lastmessage, w_chat.l.l, sizeof(lastmessage));
                    plr->message = lastmessage;
                    // Send full message over network (bypasses broken ticcmd chatchar)
                    if (netgame)
                        D_SendChatMessage(consoleplayer, w_chat.l.l);
                }
	    }
	    else if (c == KEY_ESCAPE)
	    {
                StopChatInput();
            }
	}
    }

    return eatkey;
}
