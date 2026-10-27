LOCAL BLOODBORNE BACKEND
========================

Lets Bloodborne connect and play co-op without thehuntersdream.com.

Normally the game checks in with a community server on the internet before it
will go online. If that server is down or slow, the game drops to offline mode
and nobody can summon anyone. This replaces that check with a small server one
of you runs, so going online only depends on your own PCs.

Co-op is unaffected. Bell summoning already runs through your own shadNet, and
once two players are connected the gameplay itself is direct PC to PC.

shadNet now answers all of this itself (unless BloodborneBackend=false in
server\shadnet.cfg). Then this Python server is not
needed; "HOST - Start Servers" starts it only when nothing answers port 20443.


TWO WAYS TO RUN IT
------------------

When the host starts the server it asks one question: keep content?

  Y  Messages, bloodstains, wandering ghosts, lantern counts and chalice
     dungeon glyphs are saved on the host PC and shown to the other players.
     Your world fills up with what your group leaves behind.

  N  Everyone is online and co-op works, but the world stays empty. No
     messages, no bloodstains, no ghosts.

Either way the chalice rituals work: a standard chalice's ritual (Pthumeru
Chalice, Hintertomb Chalice and the like) asks the server for that
chalice's dungeon, and the server makes it itself. Without it the game said
"Failed to contribute ritual materials" and went back to the title.

Either way there is nothing from the wider community: with this server you
only ever see content from the people on your own server.


INSTALL
-------

Put this whole LocalServer folder directly inside your Bloodborne PC folder,
next to "LAUNCH - shadPS4 Launcher.bat". Do not rename it or move the files
inside it - the tools find your settings by looking one folder up.

You need Python installed. Get it from https://www.python.org/downloads/ and
tick "Add python.exe to PATH" during setup. Only the host needs Python.


WHO DOES WHAT
-------------

HOST - the person whose PC runs shadNet:

  1. Start shadNet as usual ("SERVER - Host P2P").
  2. Run  SERVER - Start Local Backend
     Enter your own IP, the same one your friends use in shadPS4.
     Answer the "keep content" question (and, if Y, whether you want to see
     your own bloodstains too - handy when testing alone).
     Leave the window open the whole time you play.
  3. Run  SETUP - Switch To Local Backend
     Enter the same IP.
  4. Start the game.

EVERYONE ELSE:

  1. Run  SETUP - Switch To Local Backend
     Enter the HOST's IP.
  2. Start the game.

  You do not run the server and you do not need Python.

Your answers are remembered, so after the first time it is just Enter at
each prompt.


WELCOME MESSAGE
---------------

The game shows a notice when you connect. Edit  server\notice.txt  to change
it: the first line is the title, everything after it is the message. Save the
file and it takes effect on the next connection - no restart needed. Empty the
file to show nothing.

This one belongs to the sign-in screen. The game asks for it only when you log
in, and it remembers which notices it has already displayed, so the window
appears once and then stops - even after restarting the server or the PC.

Editing notice.txt makes it count as a new message, so it shows again. Leaving
the text alone means nobody is shown the same thing every session. There is no
way to force a repeat of an unchanged message; the game decides that, not the
server.

To say something to people who are already playing, use TOOL - Send Message
instead - that is a different window and it appears mid-game.

To also show a message on the login screen itself, create
server\login_message.txt  with the text you want.


GOING BACK
----------

Run  SETUP - Restore Hunters Dream  and enter the shadNet IP. That puts the
original settings back and you can stop the local backend. Everyone who wants
to go back has to run it on their own PC.

Your previous settings are also saved automatically as host_overrides.json.backup
next to the real file, in case you want to look at them.


SENDING A MESSAGE WHILE PEOPLE PLAY
-----------------------------------

Run  TOOL - Send Message  and type a title and a line of text. It reaches
everyone in game within about a minute - they do not have to restart or log
back in. Leave the title blank to clear a pending message instead.

Each distinct message is shown once per player. Sending the same wording twice
shows nothing the second time; change the text to send it again. Restarting
the server also re-sends whatever is pending, which is handy after "back up
in 5 minutes".

The message lives in  server\broadcast.txt  (first line title, rest body), so
you can edit that file directly instead if you prefer. If the server is not
running, the message simply waits there and goes out when it starts.

NOTE: this one is a best guess. Every other part of this server was built from
recordings of the real one, but the community server never actually sent a
popup while we were watching, so the exact format could not be confirmed. If
nothing appears in game, that is why - nothing breaks either way.


SEEING WHAT IS STORED
---------------------

Run  TOOL - Show Content  while the server is running. It asks the server and
prints how many players, messages, bloodstains, ghosts and dungeons it holds.


CLEARING OR BACKING UP CONTENT
------------------------------

Everything the server keeps lives in one file:  server\content.db

  Back up   copy that file somewhere while the server is stopped.
  Clear     run  TOOL - Clear Content  (stop the server first). This wipes
            every message, bloodstain, ghost and dungeon. Player accounts are
            recreated automatically at the next login.
  Move      copy content.db to the new host PC's LocalServer\server folder.

IMPORTANT: never open server\content.db - or copy it - while the server is
running. The server holds that file open, and a second program touching it can
reset it and lose everything stored. Stop the server first, or use
TOOL - Show Content, which asks the server instead of reading the file.


CHECKING IT WORKS
-----------------

The server window prints every request the game makes. When a player connects
you should see lines like:

  ss.info -> 14876 bytes
  login: YourName -> UserId 1
  /blood_messenger/message_area -> BloodMessGetListResponse  [BloodMessList=3]

You can also open this in a browser on any PC, using the host's IP:

  http://HOST-IP:20443/status

A working server replies with  {"ok": true, ...}  and, when content is kept,
how many messages, bloodstains and dungeons it holds.


IF SOMETHING GOES WRONG
-----------------------

Game still says offline
  - Is the server window still open on the host PC? It has to stay running
    the whole session; the game re-checks every 5 minutes.
  - Allow TCP port 20443 through the host's firewall.
  - Everyone must have run "SETUP - Switch To Local Backend" with the HOST's
    IP, not their own.
  - Restart shadPS4 after switching. It only reads the setting at startup.

Can't summon
  - That part is unchanged by this tool. Check shadNet is running and that
    your shadPS4 network settings still point at it, same as before.

I can't see my friend's messages
  - Was the server started with "keep content" = Y? The window says
    "content -> off" if not.
  - Messages only appear in the area they were written in, same as normal.

The server window shows "NOTE: unmapped service ..."
  - The game asked for something not yet implemented. It still gets a valid
    answer, so nothing breaks. Pass the line on if you want it added.

Want out
  - Run "SETUP - Restore Hunters Dream". Nothing else on your install has been
    touched.


WHAT THIS DOES AND DOES NOT TOUCH
---------------------------------

Changes one file:  DATA\user\host_overrides.json
Backs it up first: DATA\user\host_overrides.json.backup

Everything else it creates stays inside this LocalServer folder:
  server\content.db      stored content (only with "keep content" = Y)
  server\server-ip.txt   the IP you entered
  server\options.bat     your answers to the start-up questions

Your saves, mods, shadNet database, launcher settings and existing tools are
all left alone.
