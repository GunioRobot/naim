Quickstart guide for naim-oscar
===============================

Before you begin, you'll need the most recent Luaified naim.  To get that,
clone from [jwise/naim](http://github.com/jwise/naim) and run `autogen.sh`.

Next, check this repo out someplace memorable.  On my system, it's in
`/Users/joshua/naim-git/naim/oscar` -- since naim doesn't have a reasonable
Lua search path yet, this is a quick hack.

Now, create a file in your homedir, `init.lua`, or something like that;
inside it, put something like:

	dofile"naim-git/naim/oscar/numutil.lua"
	dofile"naim-git/naim/oscar/FLAP.lua"
	dofile"naim-git/naim/oscar/SNAC.lua"
	dofile"naim-git/naim/oscar/TLV.lua"
	dofile"naim-git/naim/oscar/OSCAR.lua"

Start up the new naim, and `/dofile init.lua`.  Then, `/newconn you OSCAR`;
`/open :RAW` if you want debug messages to go into one window; and then
`/connect` as you would ordinarily.  For the time being, I advise against
doing this on an account you care about the buddy list of; SSI buddy support
is still very unstable.

Enjoy!

`joshua`