DESKTOP_FILE=ClassiCube0.desktop
GAME_DIR=`pwd`

# remove existing ClassiCube0 desktop entry file
rm $DESKTOP_FILE

# download ClassiCube0 icon from github if necessary
if [ -f "CC0icon.png" ]
then
  echo "CC0icon.png exists already. Skipping download."
else
  echo "CC0icon.png doesn't exist. Attempting to download it.."
  wget "https://raw.githubusercontent.com/RandomStrangers/ClassiCube-Tophat/master/misc/CCicon.png"
fi

# create ClassiCube0 desktop entry
echo 'Creating ClassiCube0.desktop..'
cat >> $DESKTOP_FILE << EOF
[Desktop Entry]
Type=Application
Comment=Minecraft Classic inspired sandbox game
Name=ClassiCube0
Exec=$GAME_DIR/ClassiCube0
Icon=$GAME_DIR/CC0icon.png
Path=$GAME_DIR
Terminal=false
Categories=Game;
Actions=singleplayer;resume;

[Desktop Action singleplayer]
Name=Start singleplayer
Exec=$GAME_DIR/ClassiCube0 --singleplayer

[Desktop Action resume]
Name=Resume last server
Exec=$GAME_DIR/ClassiCube0 --resume
EOF
chmod +x $DESKTOP_FILE

echo 'Installing ClassiCube0.desktop..'
# install ClassiCube0 desktop entry into the system
sudo desktop-file-install --dir=/usr/share/applications ClassiCube0.desktop
sudo update-desktop-database /usr/share/applications
