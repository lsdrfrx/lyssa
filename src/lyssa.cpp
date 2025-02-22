#include "config.hpp" 
#include "log.hpp"
#include "playlists.hpp"
#include "popups.hpp"
#include "soundHandler.hpp"
#include "soundTagParser.hpp"
#include "window.hpp"
#include "utils.hpp"
#include "global.hpp"
#include "random.hpp"

#include <cglm/types-struct.h>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
  #include <leif/leif.h>
}

#include <GL/gl.h>
#include <GLFW/glfw3.h>

#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/mpegheader.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/tfile.h>

#include <miniaudio.h>

#include <stdlib.h>
#include <string.h>


#ifdef _WIN32
#define HOMEDIR "USERPROFILE"
#else
#define HOMEDIR "HOME"
#endif

#define LYSSA_DIR std::string(getenv(HOMEDIR)) + std::string("/.lyssa")


#define LF_PTR (vec2s){lf_get_ptr_x(), lf_get_ptr_y()}

#define MAX_PLAYLIST_NAME_LENGTH 16
#define MAX_PLAYLIST_DESC_LENGTH 512 

#define INPUT_BUFFER_SIZE 512


using namespace TagLib;


static void                     winResizeCb(GLFWwindow* window, float width, float height);
static void                     winFocusCb(GLFWwindow* window, int focused);
static void                     initWin(float width, float height);
static void                     initUI();
static void                     handleTabKeyStrokes();

static void                     renderDashboardNav();
static void                     renderCreatePlaylist(std::function<void()> onCreateCb = nullptr, std::function<void()> clientUICb = nullptr, std::function<void()> backButtonCb = nullptr);
static void                     renderCreatePlaylistFromFolder();
static void                     renderDownloadPlaylist();
static void                     renderOnPlaylist();
static void                     renderOnTrack();
static void                     renderTrackFullscreen();
static void                     renderPlaylistAddFromFile();
static void                     renderPlaylistAddFromFolder();
static void                     renderPlaylistSetThumbnail();
static void                     renderSearchPlaylist();
static void                     renderSearchAll();

static void                     renderFileDialogue(
    std::function<void(std::filesystem::directory_entry)> clickedEntryCb, 
    std::function<void()> clickedBackCb, 
    std::function<void()> renderTopBarCb, 
    std::function<void(std::filesystem::directory_entry, bool)> renderIconCb, 
    std::function<bool(std::filesystem::directory_entry, bool)> renderPerEntryCb, 
    const std::vector<std::filesystem::directory_entry>& folderContents,
    const std::vector<std::string>& selectedExtensions = {},
    bool renderDirectoriesOnly = false);

static void                     renderTrackDisplay();

static void                     renderTrackVolumeControl();
static void                     renderTrackProgress(bool dark = false);
static void                     renderTrackMenu();

static void                     beginBottomNavBar();

static void                     updateFullscreenTrackTab();

static void                     backButtonTo(GuiTab tab, const std::function<void()>& clickCb = nullptr);

static void                     loadPlaylists();
static void                     loadPlaylistFileAsync(std::vector<SoundFile>* files, std::string path);
static void                     addFileToPlaylistAsync(std::vector<SoundFile>* files, std::string path, uint32_t playlistIndex);

static void                     moveFileInPlaylistIdx(uint32_t playlistIndex, uint32_t fromIndex, uint32_t toIndex);

static std::vector<std::string> loadFilesFromFolder(const std::filesystem::path& folderPath);
static void                     playlistPlayFileWithIndex(uint32_t i, uint32_t playlistIndex);

static void                     skipSoundUp(uint32_t playlistIndex);
static void                     skipSoundDown(uint32_t playlistIndex);

static std::string              formatDurationToMins(int32_t duration);
static void                     updateSoundProgress();

static std::string              removeFileExtension(const std::string& filename);
static std::string              removeFileExtensionW(const std::string& filename);

static void                     terminateAudio();

static void                     loadIcons();

static void                     handleAsyncPlaylistLoading();
static void                     loadPlaylistAsync(Playlist& playlist);

static LfClickableItemState     renderSoundFileThumbnail(vec2s thumbnailContainerSize, SoundFile& file, 
                                                          const std::function<void()>& clickCb = nullptr, bool uiResponse = true, float cornerRadius = -1.0f);

static bool                     renderMenuBarElement(const std::string& text, uint32_t iconId);

static std::vector<SoundFile>   matchSoundFiles(const std::vector<SoundFile>& files, const std::string& searchTerm);
static void                     searchPlaylistInputInsertCb(void* inputData);
static void                     searchPlaylistInputKeyCb(void* inputData);

static LfTextProps              renderTextRaw(vec2s pos, const std::string& text, LfFont font, LfColor color, float wrapPoint = -1.0f, vec2s stopPoint = (vec2s){-1.0f, -1.0f}, bool noRender = false);

std::vector<std::filesystem::directory_entry> loadFolderContents(const std::string& folderpath) {
  std::vector<std::filesystem::directory_entry> contents;
  for (const auto& entry : std::filesystem::directory_iterator(folderpath)) {
    contents.emplace_back(entry);
  }
  std::sort(contents.begin(), contents.end());
  return contents;
}

void winResizeCb(GLFWwindow* window, int32_t width, int32_t height) {
  lf_resize_display(width, height);
  glViewport(0, 0, width, height);
  state.win->setWidth(width);
  state.win->setHeight(height);
}
void winFocusCb(GLFWwindow* window, int focused) {
  state.win->setFocused((bool)focused);
  if(state.currentTab == GuiTab::TrackFullscreen) {
    state.trackFullscreenTab.showUI = focused;
  }
}
void initWin(float width, float height) {
  if(!glfwInit()) {
    LOG_ERROR("Failed to initialize GLFW.\n");
  }

  state.win = new Window("Lyssa Music Player", (uint32_t)width, (uint32_t)height);

  lf_init_glfw(width, height, state.win->getRawWindow());   
  lf_set_text_wrap(true);
  lf_set_theme(ui_theme());

  glfwSetFramebufferSizeCallback(state.win->getRawWindow(), winResizeCb);
  glfwSetWindowFocusCallback(state.win->getRawWindow(), winFocusCb);


  glViewport(0, 0, width, height);

  state.currentSoundPos = 0.0;
}

void initUI() {
  state.h1Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter-bold.ttf").c_str(), 48);
  state.h2Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter-bold.ttf").c_str(), 40);
  state.h3Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter-bold.ttf").c_str(), 36);
  state.h4Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter.ttf").c_str(), 30);
  state.h5Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter.ttf").c_str(), 24);
  state.h5BoldFont = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter-bold.ttf").c_str(), 24);
  state.h6BoldFont = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter-bold.ttf").c_str(), 20);
  state.h6Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter.ttf").c_str(), 20);
  state.h7Font = lf_load_font(std::string(LYSSA_DIR + "/assets/fonts/inter.ttf").c_str(), 18);
  state.musicTitleFont = lf_load_font_ex(std::string(LYSSA_DIR + "/assets/fonts/inter-bold.ttf").c_str(), 72, 3072, 3072); 

  state.currentTab = GuiTab::Dashboard;

  state.infoCards = InfoCardHandler(15.0f, 60.0f);

  state.searchPlaylistInput.input = (LfInputField){
    .width = 400, 
      .buf = state.searchPlaylistInput.buffer, 
      .buf_size = INPUT_BUFFER_SIZE,  
      .placeholder = (char*)"Search for any song",
      .key_callback = searchPlaylistInputKeyCb,
  };

  loadIcons();

  state.createPlaylistTab.nameInput.input = (LfInputField){
    .width = 600, 
      .buf = state.createPlaylistTab.nameInput.buffer, 
      .buf_size = INPUT_BUFFER_SIZE,  
      .placeholder = (char*)"Name",
  };

  state.createPlaylistTab.descInput.input = (LfInputField){
    .width = 600, 
      .buf = state.createPlaylistTab.descInput.buffer, 
      .buf_size = INPUT_BUFFER_SIZE,  
      .placeholder = (char*)"Description",
  };

  state.playlistAddFromFileTab.pathInput.input = (LfInputField){
    .width = 600, 
      .buf = state.playlistAddFromFileTab.pathInput.buffer, 
      .buf_size = INPUT_BUFFER_SIZE,  
      .placeholder = (char*)"Path",
  };

  state.trackProgressSlider = (LfSlider){
    .val = reinterpret_cast<int32_t*>(&state.currentSoundPos) 
  };

  state.volumeSlider = (LfSlider){
    .val = reinterpret_cast<int32_t*>(&state.soundHandler.volume), 
      .min = 0, 
      .max = VOLUME_MAX,
      .width = 100,
      .height = 5.0f,
      .handle_size = 15
  };
}

void handleTabKeyStrokes() {
  if(lf_key_went_down(GLFW_KEY_G)) {
    Playlist& favourites = state.playlists[0]; // 0th playlist is favourites
    Playlist& currentPlaylist = state.playlists[state.currentPlaylist]; 
    std::filesystem::path selectedPath = currentPlaylist.musicFiles[currentPlaylist.playingFile].path;
    if(!Playlist::metadataContainsFile(selectedPath.string(), 0)) {
      if(favourites.loaded) {
        Playlist::addFile(selectedPath, 0);
        favourites.loaded = false;
      } else {
        std::ofstream metadata(favourites.path.string() + "/.metadata", std::ios::app);
        metadata.seekp(0, std::ios::end);

        metadata << "\"" << selectedPath.string() << "\" ";
        metadata.close();
      }
      state.infoCards.addCard("Added to favourites.");
    } else {
      if(!favourites.loaded) {
        std::vector<std::string> paths = PlaylistMetadata::getFilepaths(
            std::filesystem::directory_entry(state.playlists[0].path));
        for(auto& path : paths) {
          favourites.musicFiles.push_back((SoundFile){.path = path});
        }
        Playlist::removeFile(selectedPath.string(), 0);
        favourites.musicFiles.clear();
      } else {
        Playlist::removeFile(selectedPath.string(), 0);
      }
      state.infoCards.addCard("Removed from favourites.");
    }
  }

if(lf_key_event().pressed && lf_key_event().happened) {
    switch(lf_key_event().keycode) {
      case GLFW_KEY_SPACE:
        if(state.soundHandler.isInit)
        {
          if(state.soundHandler.isPlaying)
            state.soundHandler.stop();
          else 
            state.soundHandler.play();
        }
        break;
      case GLFW_KEY_F: 
        {
          if(state.currentTab == GuiTab::OnTrack) {
            changeTabTo(GuiTab::TrackFullscreen);
          } else {
            Playlist& currentPlaylist = state.playlists[state.currentPlaylist];
            if(currentPlaylist.playingFile == -1) return;
            state.currentSoundFile = &currentPlaylist.musicFiles[currentPlaylist.playingFile];
            if(state.onTrackTab.trackThumbnail.width != 0) {
              lf_free_texture(&state.onTrackTab.trackThumbnail);
            }
            state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path, (vec2s){-1, -1});
            changeTabTo(GuiTab::OnTrack);
          }
          break;
        }
      case GLFW_KEY_S: 
        {
          state.shuffle = !state.shuffle;
          break;
        }
      case GLFW_KEY_R: 
        {
          state.replayTrack = !state.replayTrack;
          if(state.currentTab == GuiTab::OnTrack) {
            std::string infoStr = "Replay Mode: ";
            if(state.replayTrack) {
              infoStr += "On";
            } else {
              infoStr += "Off";
            }
            state.infoCards.addCard(infoStr);
          }
          break;
        }
      case GLFW_KEY_N:
        if(state.currentPlaylist != -1) 
        {
          if(lf_key_is_down(GLFW_KEY_LEFT_SHIFT)) {
            skipSoundDown(state.currentPlaylist);
          } else {
            skipSoundUp(state.currentPlaylist);
          }
        }
        break;
      case GLFW_KEY_ENTER:
        if(state.playlists[state.currentPlaylist].selectedFile != -1)
        {
          Playlist& currentPlaylist = state.playlists[state.currentPlaylist];
          playlistPlayFileWithIndex(currentPlaylist.selectedFile, state.currentPlaylist);
          state.currentSoundFile = &currentPlaylist.musicFiles[currentPlaylist.playingFile];
          float filePosY = currentPlaylist.musicFiles[currentPlaylist.playingFile].renderPosY;
          currentPlaylist.scroll = -filePosY;
          break;
        }
      case GLFW_KEY_LEFT:
        if(state.soundHandler.isInit)
        {
          if(state.soundHandler.getPositionInSeconds() - 5 >= 0) {
            state.soundHandler.setPositionInSeconds(state.soundHandler.getPositionInSeconds() - 5);
            state.currentSoundPos = state.soundHandler.getPositionInSeconds();
          }
        }
        break;
      case GLFW_KEY_RIGHT: 
        if(state.soundHandler.isInit)
        {
          if(state.soundHandler.getPositionInSeconds() + 5 <= state.soundHandler.lengthInSeconds) {
            state.soundHandler.setPositionInSeconds(state.soundHandler.getPositionInSeconds() + 5);
            state.currentSoundPos = state.soundHandler.getPositionInSeconds();
          }
        }
        break;
      case GLFW_KEY_DOWN:
        if(state.currentTab == GuiTab::OnPlaylist)
        {
          Playlist& currentPlaylist = state.playlists[state.currentPlaylist];
          if(currentPlaylist.selectedFile + 1 < currentPlaylist.musicFiles.size()) {
            currentPlaylist.selectedFile++;
          } else {
            currentPlaylist.selectedFile = 0;
          }
          float filePosY = currentPlaylist.musicFiles[currentPlaylist.selectedFile].renderPosY;
          currentPlaylist.scroll = -filePosY;
        }
        break;
      case GLFW_KEY_UP: 
        if(state.currentTab == GuiTab::OnPlaylist)
        {
          Playlist& currentPlaylist = state.playlists[state.currentPlaylist];
          if(currentPlaylist.selectedFile - 1 >= 0) {
            currentPlaylist.selectedFile--;
          } else {
            currentPlaylist.selectedFile = currentPlaylist.musicFiles.size() - 1;
          }
          float filePosY = currentPlaylist.musicFiles[currentPlaylist.selectedFile].renderPosY;
          currentPlaylist.scroll = -filePosY;
        }
        break;
      case GLFW_KEY_V: 
        {
          if(lf_key_is_down(GLFW_KEY_LEFT_SHIFT)) {
            state.showVolumeSliderTrackDisplay = true;
            state.showVolumeSliderOverride = true;
            if(*(int32_t*)state.volumeSlider.val + VOLUME_TOGGLE_STEP <= VOLUME_MAX)
              *(int32_t*)state.volumeSlider.val += VOLUME_TOGGLE_STEP; 
            else 
              *(int32_t*)state.volumeSlider.val = VOLUME_MAX;
          } else {
            state.showVolumeSliderTrackDisplay = true;
            state.showVolumeSliderOverride = true;
            if(*(int32_t*)state.volumeSlider.val - VOLUME_TOGGLE_STEP >= 0)
              *(int32_t*)state.volumeSlider.val -= VOLUME_TOGGLE_STEP; 
            else 
              *(int32_t*)state.volumeSlider.val = 0;
          }
          break;
        }
      case GLFW_KEY_M: 
        if(state.soundHandler.isInit)
        {
          if(state.soundHandler.volume != 0.0f) 
            state.volumeBeforeMute = state.soundHandler.volume;

          state.soundHandler.volume = (state.soundHandler.volume != 0.0f) ? 0.0f : state.volumeBeforeMute;
          state.showVolumeSliderTrackDisplay = true;
          state.showVolumeSliderOverride = true;
        }
        break;
    }
  }
}

static void renderHomepage() {
  lf_push_font(&state.h1Font);
  LfUIElementProps props = lf_get_theme().text_props;
  props.text_color = LF_WHITE;
  lf_push_style_props(props);
  lf_text("Your Playlists");
  lf_pop_style_props();
  lf_pop_font();
  if(state.playlists.size() > 1)  {
    {
      const float width = 170;
      const float height = -1;
      LfUIElementProps props = primary_button_style();
      props.margin_right = 0;
      props.margin_left = 0;
      lf_push_style_props(props);
      lf_push_font(&state.h1Font);
      lf_set_ptr_x_absolute(state.win->getWidth() - ((width + props.padding * 2.0f) * 2.0f) - DIV_START_X * 2);
      lf_pop_font();

      if(lf_button_fixed("Download Playlist", width, height) == LF_CLICKED) { 
        changeTabTo(GuiTab::DownloadPlaylist);
      }
      lf_pop_style_props();
      props.margin_left = 10;
      lf_push_style_props(props);
      if(lf_button_fixed("Add Playlist", width, height) == LF_CLICKED) {
        state.popups[PopupType::TwoChoicePopup] = std::make_unique<TwoChoicePopup>(
            400, 
            "How do you want to add a Playlist?", 
            "Create New", 
            "From Folder", 
            [&](){
            changeTabTo(GuiTab::CreatePlaylist);
            state.popups[PopupType::TwoChoicePopup]->shouldRender = false;
            lf_div_ungrab();
            }, 
            [&](){
            if(state.playlistAddFromFolderTab.currentFolderPath.empty()) {
            state.playlistAddFromFolderTab.currentFolderPath = std::string(getenv(HOMEDIR));
            state.playlistAddFromFolderTab.folderContents = loadFolderContents(state.playlistAddFromFolderTab.currentFolderPath);
            }
            changeTabTo(GuiTab::CreatePlaylistFromFolder);
            state.popups[PopupType::TwoChoicePopup]->shouldRender = false;
            lf_div_ungrab();
            });
        state.popups[PopupType::TwoChoicePopup]->shouldRender = !state.popups[PopupType::TwoChoicePopup]->shouldRender;
      }
      lf_pop_style_props();
    }
  }

  lf_next_line();
  if(state.playlists.size() == 1 /* No playlist, only favourites */) {
    {
      const char* text = "You don't have any playlists.";
      float textWidth = lf_text_dimension(text).x;
      lf_set_ptr_x(((state.win->getWidth() - state.sideNavigationWidth) - textWidth) / 2.0f - DIV_START_X);
      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_top = 40;
      props.margin_left = 0;
      props.margin_right = 0;
      lf_push_style_props(props);
      lf_text(text);
      lf_pop_style_props();
    }
    lf_next_line();
    {
      const float width = 200;
      lf_set_ptr_x(((state.win->getWidth() - state.sideNavigationWidth) - ((width + (lf_get_theme().button_props.padding * 2)) + 5) * 2) / 2.0f - DIV_START_X);
      LfUIElementProps props = primary_button_style();
      props.margin_right = 5;
      props.margin_left = 5;
      props.margin_top = 15;
      props.corner_radius = 12;
      lf_push_style_props(props);
      if(lf_button_fixed("Add Playlist", width, 50) == LF_CLICKED)  {
        state.popups[PopupType::TwoChoicePopup] = std::make_unique<TwoChoicePopup>(
            400, 
            "How do you want to add a Playlist?", 
            "Create New", 
            "From Folder", 
            [&](){
            changeTabTo(GuiTab::CreatePlaylist);
            state.popups[PopupType::TwoChoicePopup]->shouldRender = false;
            lf_div_ungrab();
            }, 
            [&](){
            if(state.playlistAddFromFolderTab.currentFolderPath.empty()) {
            state.playlistAddFromFolderTab.currentFolderPath = std::string(getenv(HOMEDIR));
            state.playlistAddFromFolderTab.folderContents = loadFolderContents(state.playlistAddFromFolderTab.currentFolderPath);
            }
            changeTabTo(GuiTab::CreatePlaylistFromFolder);
            state.popups[PopupType::TwoChoicePopup]->shouldRender = false;
            lf_div_ungrab();
            });
        state.popups[PopupType::TwoChoicePopup]->shouldRender = !state.popups[PopupType::TwoChoicePopup]->shouldRender;
      }
      if(lf_button_fixed("Download Playlist", width, 50) == LF_CLICKED)  {
        changeTabTo(GuiTab::DownloadPlaylist);
      }
      lf_pop_style_props();
    }
  } else {

    lf_set_ptr_y_absolute(lf_get_ptr_y() + 30);
    lf_div_begin(LF_PTR, ((vec2s){(float)state.win->getWidth() - DIV_START_X * 2 - state.sideNavigationWidth, 
          (float)state.win->getHeight() - DIV_START_Y * 2 - lf_get_ptr_y() - 
          (BACK_BUTTON_HEIGHT + BACK_BUTTON_MARGIN_BOTTOM)}), true);

    const float margin = 20; 
    const float innerMargin = 10;
    const vec2s size = (vec2s){220, 380};
    const LfColor color = lf_color_brightness(LYSSA_BACKGROUND_COLOR, 0.8f);
    const float ptrXStart = lf_get_ptr_x();
    const float cornerRadius = 6.5f;

    for(uint32_t i = 0; i < state.playlists.size(); i++) {
      Playlist& playlist = state.playlists[i];
      if(playlist.path == LYSSA_DIR + "/playlists/favourites") continue;

      // Go to the next line on overflow
      if(lf_get_ptr_x() + size.x >= state.win->getWidth() - margin) {
        lf_set_ptr_y_absolute(lf_get_ptr_y() + size.y + margin);
        lf_set_ptr_x_absolute(ptrXStart);
      }
      vec2s containerPos = LF_PTR;
      LfAABB containerAABB = (LfAABB){.pos = containerPos, .size = size};
      bool onContainer = lf_hovered(containerAABB.pos, containerAABB.size);

      // Rendering the container
      lf_rect_render(containerPos, size, onContainer ? lf_color_brightness(color, 0.9f) : color, LF_NO_COLOR, 0.0f, cornerRadius);

      // Rendering the thumbnail
      {
        // Container
        LfAABB thumbnailAABB = (LfAABB){
          .pos = (vec2s){lf_get_ptr_x() + innerMargin, lf_get_ptr_y() + innerMargin}, 
            .size = (vec2s){size.x - innerMargin * 2.0f, size.x - innerMargin}};

        lf_rect_render(thumbnailAABB.pos, thumbnailAABB.size, 
            lf_color_brightness(GRAY, 0.5), LF_NO_COLOR, 0.0f, cornerRadius * 2.0f);

        // Thumbnail
        LfTexture thumbnail = playlist.thumbnail.width == 0 ? state.icons["music_note"] : playlist.thumbnail;

        float thumbnailAspect = (float)thumbnail.width / (float)thumbnail.height;
        float containerAspect = (float)thumbnailAABB.size.x / (float)thumbnailAABB.size.y;
        float scaleFactor;
        if (thumbnailAspect > containerAspect) {
          scaleFactor = (float)thumbnailAABB.size.x / (float)thumbnail.width;
        } else {
          scaleFactor = (float)thumbnailAABB.size.y / (float)thumbnail.height;
        }
        float thumbnailWidth = thumbnail.width * scaleFactor; 
        float thumbnailHeight = thumbnail.height * scaleFactor; 

        lf_image_render((vec2s){lf_get_ptr_x() + innerMargin, lf_get_ptr_y() + innerMargin + 
            (thumbnailAABB.size.y - thumbnailHeight) / 2.0f}, LF_WHITE, 
            (LfTexture){.id = thumbnail.id, .width = (uint32_t)thumbnailWidth, .height = (uint32_t)thumbnailHeight}, 
            LF_NO_COLOR, 0.0f, ((thumbnailHeight + cornerRadius * 4.0f > thumbnailAABB.size.y) ? cornerRadius * 2.0f : 0.0f));
      }

      lf_set_ptr_y_absolute(lf_get_ptr_y() + (size.x + innerMargin));

      // Rendering the playlist's name
      LfTextProps textPropsName;
      {
        lf_set_cull_end_x(lf_get_ptr_x() + size.x - innerMargin);
        lf_set_ptr_x_absolute(lf_get_ptr_x() + innerMargin);
        textPropsName = renderTextRaw(LF_PTR, playlist.name.c_str(), lf_get_theme().font, LF_WHITE, 
            lf_get_ptr_x() + size.x - innerMargin * 2.0f, (vec2s){-1, lf_get_ptr_y() + lf_get_theme().font.font_size * 2.0f});

        float height = textPropsName.height > lf_get_theme().font.font_size ? lf_get_theme().font.font_size * 2.0f : lf_get_theme().font.font_size;
        lf_set_ptr_y_absolute(lf_get_ptr_y() + height);
        lf_set_ptr_x_absolute(lf_get_ptr_x() - innerMargin);
        lf_unset_cull_end_x();
      }
      // Rendering the playlist's description
      LfTextProps textPropsDesc; 
      {
        lf_set_cull_end_x(lf_get_ptr_x() + size.x - innerMargin);
        lf_set_ptr_x_absolute(lf_get_ptr_x() + innerMargin);
        textPropsDesc = renderTextRaw(LF_PTR, playlist.desc.c_str(), state.h6Font, GRAY, 
            lf_get_ptr_x() + size.x - innerMargin * 2.0f, (vec2s){-1, lf_get_ptr_y() + lf_get_theme().font.font_size * 2.0f});

        float height = textPropsDesc.height > lf_get_theme().font.font_size ? lf_get_theme().font.font_size * 2.0f : lf_get_theme().font.font_size;
        lf_set_ptr_y_absolute(lf_get_ptr_y() + height);
        lf_set_ptr_x_absolute(lf_get_ptr_x() - innerMargin);
        lf_unset_cull_end_x();
      }

      // If container is hovered, render action buttons
      bool onActionButton = false;
      if(onContainer) {
        vec2s buttonSize = (vec2s){24, 24};
        LfUIElementProps props = lf_get_theme().button_props;
        props.padding = 0.0f;
        props.margin_left = innerMargin;
        props.margin_top = 0.0f;
        props.margin_bottom = 0.0f;
        props.border_width = 0.0f;
        props.color = LF_NO_COLOR;
        lf_set_image_color(lf_color_brightness(GRAY, 1.6));
        lf_push_style_props(props);
        lf_set_ptr_y_absolute(containerPos.y + size.y - (buttonSize.y + innerMargin * 2.0f));

        // Edit button
        LfClickableItemState editButton =  lf_image_button(((LfTexture){.id = state.icons["edit"].id, 
              .width = (uint32_t)buttonSize.x, .height = (uint32_t)buttonSize.y}));
        if(editButton == LF_CLICKED) {
          state.currentPlaylist = i;
          state.popups[PopupType::EditPlaylistPopup] = std::make_unique<EditPlaylistPopup>();
          state.popups[PopupType::EditPlaylistPopup]->shouldRender = true;
        }
        lf_pop_style_props();

        props.margin_left = 5.0f;
        lf_push_style_props(props);
        LfClickableItemState deleteButton = lf_image_button(((LfTexture){.id = state.icons["delete"].id, 
              .width = (uint32_t)buttonSize.x, .height = (uint32_t)buttonSize.y}));
        if(deleteButton == LF_CLICKED) {
          if(state.soundHandler.isInit) {
            state.soundHandler.stop();
            state.soundHandler.uninit();
            state.currentSoundFile = nullptr;
          }
          Playlist::remove(i);
          state.infoCards.addCard("Removed playlist.");
        }
        LfClickableItemState thumbnailButton = lf_image_button(((LfTexture){.id = state.icons["thumbnail"].id, .width = 29, .height = 26}));
        if(thumbnailButton == LF_CLICKED) {
          state.currentPlaylist = i;
          changeTabTo(GuiTab::PlaylistSetThumbnail);
        }
        onActionButton = (editButton != LF_IDLE || deleteButton != LF_IDLE || thumbnailButton != LF_IDLE);
        lf_pop_style_props();
        lf_unset_image_color();
      }

      // On Playlist enter
      if(onContainer && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT) && !onActionButton) { 
        state.currentPlaylist = i;
        if(!playlist.loaded) {
          state.loadedPlaylistFilepaths.clear();
          state.loadedPlaylistFilepaths.shrink_to_fit();

          state.loadedPlaylistFilepaths = PlaylistMetadata::getFilepaths(std::filesystem::directory_entry(playlist.path));
          loadPlaylistAsync(playlist);
          playlist.loaded = true;
        }
        changeTabTo(GuiTab::OnPlaylist);
      }

      // Advancing the pointer 
      lf_set_ptr_x_absolute(containerPos.x + size.x + margin);
      lf_set_ptr_y_absolute(containerPos.y);
    }
    lf_set_ptr_y_absolute(lf_get_ptr_y() + size.y + margin);

    lf_div_end();
  }

}

static void renderFavourites() {
  renderOnPlaylist();
}

void renderDashboard() {
  if(state.dashboardTab == DashboardTab::Home) {
    renderHomepage();
  } 
  else if(state.dashboardTab == DashboardTab::Search) {
    renderSearchAll();
  }
  else {
    renderFavourites();
  }

  if(state.dashboardTab != DashboardTab::Favourites) {
    beginBottomNavBar();
    renderTrackMenu();
  }
  lf_div_end();
}

void renderDashboardNav() {
  if(state.currentTab == GuiTab::OnTrack || state.currentTab == GuiTab::TrackFullscreen) return; 
  LfUIElementProps props = lf_get_theme().div_props;
  props.color = lf_color_brightness(LYSSA_BACKGROUND_COLOR, 0.9f);
  lf_push_style_props(props);
  lf_div_begin(((vec2s){0.0f, 0.0f}), ((vec2s){state.sideNavigationWidth, (float)state.win->getHeight()}), false);
  lf_pop_style_props();

  const uint32_t elementCount = 3;
  uint32_t icons[elementCount] = {
    state.icons["home"].id,
    state.icons["favourite"].id,
    state.icons["search"].id,
  };
  uint32_t iconsSelected[elementCount] = {
    state.icons["home_selected"].id,
    state.icons["favourite_selected"].id,
    state.icons["search_selected"].id,
  };
  const char* titles[elementCount] = {
    "Home",
    "Favourites",
    "Search"
  };

  bool deactivated = state.playlistDownloadRunning;

  for(uint32_t i = 0; i < elementCount; i++) {
    LfUIElementProps imgProps = lf_get_theme().image_props;
    imgProps.margin_top = 15.0f;
    imgProps.margin_left = 7.5f;
    uint32_t imgSize = 30;
    lf_push_style_props(imgProps);
    lf_image((LfTexture){.id = (i == (uint32_t)state.dashboardTab) ? iconsSelected[i] : icons[i], 
        .width = imgSize, .height = imgSize});
    lf_pop_style_props();

    LfUIElementProps props = lf_get_theme().text_props;
    if(deactivated) {
      props.text_color = (LfColor){100, 100, 100, 255};
    }
    else {
      if(i == (uint32_t)state.dashboardTab) {
        lf_push_font(&state.h5BoldFont);
      } else {
        props.text_color = (LfColor){175, 175, 175, 255};
      }
    }
    props.margin_top = imgProps.margin_top + (imgSize- lf_text_dimension(titles[i]).y) / 2.0f;
    lf_push_style_props(props);

    bool clickedElement = lf_hovered((vec2s){lf_get_ptr_x() + props.margin_left, lf_get_ptr_y() + props.margin_top}, 
        (vec2s){state.sideNavigationWidth - props.margin_left * 2.0f, imgSize + props.margin_top}) && 
        lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT);

    lf_button(titles[i]);
    if(clickedElement && !deactivated) {
      switch((DashboardTab)i) {
        case DashboardTab::Home: 
          {
            state.dashboardTab = (DashboardTab)i;
            loadPlaylists();
            state.dashboardTab = DashboardTab::Home;
            changeTabTo(GuiTab::Dashboard);
            break;
          }
        case DashboardTab::Favourites: 
          {
            state.currentPlaylist = 0; // 0 is allocated for favourites
            Playlist& favourites = state.playlists[state.currentPlaylist];
            if(!favourites.loaded) {
              state.loadedPlaylistFilepaths.clear();
              state.loadedPlaylistFilepaths.shrink_to_fit();

              state.loadedPlaylistFilepaths = PlaylistMetadata::getFilepaths(std::filesystem::directory_entry(favourites.path));
              loadPlaylistAsync(favourites);
              favourites.loaded = true;
            }
            state.dashboardTab = (DashboardTab)i;
            changeTabTo(GuiTab::Dashboard);
            break;
          }
        case DashboardTab::Search: 
          {
            state.dashboardTab = (DashboardTab)i;
            break;
          }
      }
    }
    lf_pop_style_props();
    lf_pop_font();
    lf_next_line();
  }
 
  lf_div_end();
}

void renderCreatePlaylist(std::function<void()> onCreateCb, std::function<void()> clientUICb, std::function<void()> backButtonCb) {
  // Heading
  {
    LfUIElementProps props = lf_get_theme().text_props;
    props.text_color = LF_WHITE;
    props.margin_bottom = 15;
    lf_push_style_props(props);
    lf_push_font(&state.h1Font);
    lf_text("Create Playlist");
    lf_pop_style_props();
    lf_pop_font();
  }
  // Form Input
  {
    lf_next_line();
    state.createPlaylistTab.nameInput.input.width = state.win->getWidth() / 2.0f;
    LfUIElementProps props = input_field_style();
    lf_push_style_props(props);
    lf_input_text(&state.createPlaylistTab.nameInput.input);
    lf_pop_style_props();
    lf_next_line();
  }
  {
    lf_next_line();
    state.createPlaylistTab.descInput.input.width = state.win->getWidth() / 2.0f;
    LfUIElementProps props = input_field_style();
    lf_push_style_props(props);
    lf_input_text(&state.createPlaylistTab.descInput.input);
    lf_pop_style_props();
    lf_next_line();
  }

  // Thumbnail
  {
    lf_push_font(&state.h3Font);
    lf_text("Thumbnail");
    lf_pop_font();

    if(state.createPlaylistTab.thumbnailPath.string().empty()) {
      LfUIElementProps props = secondary_button_style();
      lf_push_style_props(props);
      if(lf_button("Select") == LF_CLICKED) {
        changeTabTo(GuiTab::PlaylistSetThumbnail);
      }
      lf_pop_style_props();
    } else {
      lf_next_line();
      static LfTexture preview{0};
      if(preview.width == 0) {
        preview = lf_load_texture(
            state.createPlaylistTab.thumbnailPath.string().c_str(),
            false, 
            LF_TEX_FILTER_LINEAR);
      }
      vec2s thumbnailContainerSize = (vec2s){180, 180};

      float aspect = (float)preview.width / (float)preview.height;
      float thumbnailHeight = MIN(thumbnailContainerSize.y / aspect, thumbnailContainerSize.y); 
      LfUIElementProps props = lf_get_theme().button_props;
      props.color = GRAY;
      props.border_width = 0.0f;
      props.corner_radius = 4.0f;
      props.padding = 0.0f;
      props.margin_left = 5.0f;
      props.margin_top = 20.0f;
      props.margin_right = 0.0f;
      props.margin_bottom = 10.0f;
      lf_push_style_props(props);
      LfClickableItemState thumbnailState = lf_item(thumbnailContainerSize);
      if(thumbnailState == LF_CLICKED) {
        changeTabTo(GuiTab::PlaylistSetThumbnail);
        lf_free_texture(&preview);
      }
      lf_pop_style_props();

      lf_image_render((vec2s){lf_get_ptr_x() - thumbnailContainerSize.x - props.margin_left, lf_get_ptr_y() + 
          (thumbnailContainerSize.y - thumbnailHeight) / 2.0f + props.margin_top}, LF_WHITE,
          (LfTexture){.id = preview.id, .width = 
          (uint32_t)thumbnailContainerSize.x, .height = (uint32_t)thumbnailHeight}, LF_NO_COLOR, 0.0f, 4.0f);  

      lf_next_line();

      {
        LfUIElementProps props = secondary_button_style();
        props.margin_bottom = 15.0f;
        lf_push_style_props(props);
        if(lf_button("Unselect") == LF_CLICKED) {
          state.createPlaylistTab.thumbnailPath = "";
          lf_free_texture(&preview);
        }
        lf_pop_style_props();
      }
    }
  }

  // Create Button
  {
    lf_next_line();
    LfUIElementProps props = call_to_action_button_style();
    props.margin_top = 10;
    lf_push_style_props(props);
    if(lf_button_fixed("Create", 150, -1) == LF_CLICKED) {
      FileStatus status = Playlist::create(std::string(state.createPlaylistTab.nameInput.buffer), std::string(state.createPlaylistTab.descInput.buffer), "",
          state.createPlaylistTab.thumbnailPath); 

      switch(status) {
        case FileStatus::Failed:
          state.infoCards.addCard("Failed to create playlist.", LYSSA_RED);
          break;
        case FileStatus::AlreadyExists:
          props.text_color = LYSSA_RED;
          lf_push_style_props(props);
          state.infoCards.addCard("Playlist already exists.", LYSSA_RED);
          lf_pop_style_props();
          break;
        case FileStatus::Success:
          state.infoCards.addCard("Successfully created playlist.", LYSSA_GREEN, LF_BLACK);
          break;
        default:
          break;
      }
      memset(state.createPlaylistTab.nameInput.input.buf, 0, INPUT_BUFFER_SIZE);
      memset(state.createPlaylistTab.nameInput.buffer, 0, INPUT_BUFFER_SIZE);

      memset(state.createPlaylistTab.descInput.input.buf, 0, INPUT_BUFFER_SIZE);
      memset(state.createPlaylistTab.descInput.buffer, 0, INPUT_BUFFER_SIZE);

      state.createPlaylistTab.thumbnailPath = "";
      if(onCreateCb) 
        onCreateCb();
    }
    lf_pop_style_props();
  }

  if(clientUICb)
    clientUICb();


  beginBottomNavBar();
  backButtonTo(GuiTab::Dashboard, [&](){
      loadPlaylists();
      if(backButtonCb)
      backButtonCb();
      });
  renderTrackMenu();
  lf_div_end();
}

void renderCreatePlaylistFromFolder() {
  static bool selectedFolder = false;

  if(!selectedFolder) {
    // Heading
    {
      LfUIElementProps props = lf_get_theme().text_props;
      props.text_color = LF_WHITE;
      props.margin_bottom = 15;
      lf_push_style_props(props);
      lf_push_font(&state.h1Font);
      lf_text("Create Playlist from Folder");
      lf_pop_style_props();
      lf_pop_font();
      lf_next_line();
      lf_text("Select a Folder from which to add the files.");
    }
    lf_next_line();
    renderFileDialogue(
        [&](std::filesystem::directory_entry entry){
        if(entry.is_directory()) {
        PlaylistAddFromFolderTab& tab = state.playlistAddFromFolderTab;
        tab.currentFolderPath = entry.path().string();
        tab.folderContents.clear();
        tab.folderContents = loadFolderContents(tab.currentFolderPath);
        lf_set_current_div_scroll(0.0f);
        lf_set_current_div_scroll_velocity(0.0f);
        }
        },
        [&](){
        PlaylistAddFromFolderTab& tab = state.playlistAddFromFolderTab;
        tab.currentFolderPath = std::filesystem::path(tab.currentFolderPath).parent_path().string();
        tab.folderContents.clear();
        tab.folderContents = loadFolderContents(tab.currentFolderPath); 
        },
        nullptr,
        [&](std::filesystem::directory_entry entry, bool hovered){
        LfUIElementProps props = lf_get_theme().button_props;
        props.margin_top = 0.0f;
        props.color = LF_NO_COLOR;
        props.padding = 2.5f;
        props.border_width = 0.0f;
        lf_set_image_color(LF_WHITE);
        lf_push_style_props(props);
        const vec2s iconSize = (vec2s){25, 25};
        LfTexture icon = (LfTexture){
          .id = (entry.is_directory()) ? state.icons["folder"].id : state.icons["file"].id, 
            .width = (uint32_t)iconSize.x, 
            .height = (uint32_t)iconSize.y
        };
        lf_image_button(icon);
        lf_pop_style_props();
        lf_unset_image_color();
        }, 
        [&](std::filesystem::directory_entry entry, bool hovered){
          if(hovered) {
            LfUIElementProps props = primary_button_style();
            props.margin_top = 1.5;
            props.padding = 5;
            lf_push_style_props(props);
            LfClickableItemState button = lf_button_fixed("Select", 100, -1);
            if(button == LF_CLICKED) {
              selectedFolder = true;
              state.playlistAddFromFolderTab.currentFolderPath = entry.path().string();
              strcpy(state.createPlaylistTab.nameInput.buffer, entry.path().filename().c_str());
              strcpy(state.createPlaylistTab.nameInput.input.buf, entry.path().filename().c_str());
            }
            lf_pop_style_props();
            return button != LF_IDLE;
          }
          return false;
        }, 
        state.playlistAddFromFolderTab.folderContents, {}, true);
    beginBottomNavBar();
    backButtonTo(GuiTab::Dashboard, [&](){
        loadPlaylists();
        });
    renderTrackMenu();
    lf_div_end();
  } else {
    renderCreatePlaylist([&](){
        loadPlaylists();
        PlaylistAddFromFolderTab& tab = state.playlistAddFromFolderTab;
        uint32_t playlist = state.playlists.size() - 1;
        std::ofstream metadata(state.playlists[playlist].path.string() + "/.metadata", std::ios::app);
        metadata.seekp(0, std::ios::end);
        for(const auto& entry : std::filesystem::directory_iterator(tab.currentFolderPath)) {
        if(!entry.is_directory() && SoundTagParser::isValidSoundFile(entry.path().string())) {
        metadata << "\"" << entry.path().string() << "\" ";
        }
        }
        metadata.close();
        }, 
        [&](){
        LfUIElementProps props = call_to_action_button_style();
        props.margin_top = 10;
        props.color = LYSSA_RED;
        props.text_color = LF_WHITE;
        lf_push_style_props(props);
        if(lf_button_fixed("Change Folder", 150, -1) == LF_CLICKED) {
        selectedFolder = false;
        }
        lf_pop_style_props();
        }, 
        [&](){
          selectedFolder = false;
        });
  }

}
static std::string removeSpecialCharactersStr(const std::string& input) {
    std::string result;
    for (char ch : input) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            result += ch;
        }
    }
    return result;
}

void renderDownloadPlaylist() {
  std::string downloadedPlaylistDir = LYSSA_DIR + "/downloaded_playlists/" + state.downloadingPlaylistName; 
  uint32_t downloadedFileCount = LyssaUtils::getLineCountFile(downloadedPlaylistDir + "/archive.txt");

  static std::string url;

  if(state.playlistDownloadFinished) {
    // Heading
    {
      LfUIElementProps props = lf_get_theme().text_props;
      props.text_color = LF_WHITE;
      lf_push_style_props(props);
      lf_push_font(&state.h3Font);

      std::string text = "Playlist Download Complete.";
      lf_text(text.c_str());
      lf_pop_style_props();
      lf_pop_font();
    }

    {
      lf_next_line();
      lf_push_font(&state.h6Font);
      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_top = 15;
      props.color = GRAY;
      lf_push_style_props(props);
      std::string text = "Downloading of playlist \"" + state.downloadingPlaylistName + "\" with " + std::to_string(state.downloadPlaylistFileCount) + " files finished.";
      lf_text(text.c_str());
      lf_pop_style_props();
      lf_pop_font();
    }

    {
      lf_next_line();
      LfUIElementProps props = call_to_action_button_style();
      props.margin_left = 0;
      props.margin_top = 15;
      props.color = LYSSA_BACKGROUND_COLOR;
      props.text_color = LF_WHITE;
      props.border_color = GRAY;
      props.border_width = 1.0f;
      props.corner_radius = 9.0f;
      props.margin_right = 0; 

      lf_push_style_props(props);
      if(lf_button_fixed("Open Playlist", 180, -1) == LF_CLICKED) {
        loadPlaylists();
        state.playlistDownloadRunning = false;
        state.playlistDownloadFinished = false;
        uint32_t playlistIndex = 0;
        for(uint32_t i = 0; i < state.playlists.size(); i++) {
          if(state.playlists[i].name == state.downloadingPlaylistName) {
            playlistIndex = i;
            break;
          }
        }
        state.currentPlaylist = playlistIndex;
        auto& playlist = state.playlists[state.currentPlaylist];
        if(!playlist.loaded) {
          state.loadedPlaylistFilepaths.clear();
          state.loadedPlaylistFilepaths.shrink_to_fit();

          state.loadedPlaylistFilepaths = PlaylistMetadata::getFilepaths(std::filesystem::directory_entry(playlist.path));
          loadPlaylistAsync(playlist);
          playlist.loaded = true;
        }
        changeTabTo(GuiTab::OnPlaylist);
      }
      lf_pop_style_props();
    }
    beginBottomNavBar();
    backButtonTo(GuiTab::Dashboard, [&](){
        state.playlistDownloadRunning = false;
        state.playlistDownloadFinished = false;
        loadPlaylists();
        });
    renderTrackMenu();
    lf_div_end();
    return;
  }
  if(!state.playlistDownloadRunning) {
    // Heading
    {
      LfUIElementProps props = lf_get_theme().text_props;
      props.text_color = LF_WHITE;
      props.margin_bottom = 15;
      lf_push_style_props(props);
      lf_push_font(&state.h1Font);
      lf_text("Download Playlist");
      lf_pop_style_props();
      lf_pop_font();
    }

    {
      lf_next_line();
      lf_push_font(&state.h6Font);
      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_top = 0;
      props.color = GRAY;
      lf_push_style_props(props);
      lf_text("Download a playlist from a streaming service");
      lf_pop_style_props();
      lf_pop_font();
    }

    lf_next_line();
    static char urlInput[INPUT_BUFFER_SIZE] = {0};

    {
      LfUIElementProps props = input_field_style();
      props.margin_top = 15;
      lf_push_style_props(props);
      lf_input_text_inl_ex(urlInput, INPUT_BUFFER_SIZE, 600, "URL");
      lf_pop_style_props();
    }

    lf_next_line();

    {
      lf_push_style_props(call_to_action_button_style());
      if(lf_button_fixed("Download", 150, -1) == LF_CLICKED) {
        state.downloadingPlaylistName = LyssaUtils::getCommandOutput(
            std::string("yt-dlp \"" + std::string(urlInput) + "\" --flat-playlist --dump-single-json --no-warnings | jq -r .title &"));
        state.downloadingPlaylistName = removeSpecialCharactersStr(state.downloadingPlaylistName); 

        if(state.downloadingPlaylistName != "null") {
          std::string downloadCmd = LYSSA_DIR + "/scripts/download.sh \"" + urlInput + "\" " + LYSSA_DIR + "/downloaded_playlists/ &"; 
          system(downloadCmd.c_str());
          state.playlistDownloadRunning = true;
          state.downloadPlaylistFileCount = LyssaUtils::getPlaylistFileCountURL(urlInput);
          url = urlInput;
          memset(urlInput, 0, INPUT_BUFFER_SIZE);
        } else {
          LOG_ERROR("Invalid URL Provided.");
          memset(urlInput, 0, INPUT_BUFFER_SIZE);
        }
      }
      lf_pop_style_props();
    }
  } else  {
    static float ytdlpDownTimer = 0.0f;
    if(LyssaUtils::getCommandOutput("pgrep yt-dlp") == "") {
      ytdlpDownTimer += state.deltaTime;
      if(ytdlpDownTimer >= 2.0f) {
        ytdlpDownTimer = 0.0f;
        state.playlistDownloadFinished = true;
      }
    } else {
      ytdlpDownTimer = 0.0f;
    }
      state.downloadPlaylistFileCount == LyssaUtils::getLineCountFile(LYSSA_DIR + "/downloaded_playlists/" + state.downloadingPlaylistName + "/archive.txt");

    if(state.playlistDownloadFinished) {
      FileStatus createStatus = Playlist::create(state.downloadingPlaylistName, "Downloaded Playlist", url);

      if(createStatus != FileStatus::AlreadyExists) {
        std::string playlistDir = LYSSA_DIR + "/playlists/" + state.downloadingPlaylistName; 
        std::ofstream metadata(playlistDir + "/.metadata", std::ios::app);

        metadata.seekp(0, std::ios::end);

        for (const auto& entry : std::filesystem::directory_iterator(downloadedPlaylistDir)) {
          if (entry.is_regular_file() && entry.path().extension() == ".mp3") {
            metadata << "\"" << entry.path().string() << "\" ";
          }
        }
        metadata.close();
      }
      std::string downloadThumbnailCmd = "yt-dlp --playlist-items 1 --skip-download --convert-thumbnails jpg --write-thumbnail -o \"" 
        + LYSSA_DIR + "/playlists/" + state.downloadingPlaylistName + "/thumbnail.jpg\" " + url + " &";
      system(downloadThumbnailCmd.c_str());
    }

    {
      std::string title = std::string("Downloading " + state.downloadingPlaylistName + "...");
      LfUIElementProps props = lf_get_theme().text_props;
      props.text_color = LF_WHITE;
      props.margin_bottom = 10;
      lf_push_style_props(props);
      lf_push_font(&state.h3Font);
      lf_text(title.c_str());
      lf_pop_style_props();
      lf_pop_font();
      lf_next_line();

      std::string subtitle = std::string("This can take a while.");
      props.margin_bottom = 15;
      lf_push_style_props(props);
      lf_text(subtitle.c_str());
      lf_pop_style_props();
      lf_next_line();

    }
    lf_next_line();
    {
      LfUIElementProps props = lf_get_theme().div_props;
      props.color = lf_color_brightness(LYSSA_BACKGROUND_COLOR, 1.5f);
      props.corner_radius = 6.0f; 
      lf_push_style_props(props);
      lf_div_begin(LF_PTR, ((vec2s){440, 120}), false);
      lf_pop_style_props();
    }
    {
      int percentage = (downloadedFileCount * 100) / state.downloadPlaylistFileCount;
      std::ostringstream oss;
      oss << percentage << "%";

      std::string percentStr = oss.str();
      LfUIElementProps text_props = lf_get_theme().text_props;
      text_props.margin_left = 15.0f;
      text_props.margin_bottom = 15.0f;
      lf_push_style_props(text_props);
      lf_push_font(&state.h4Font);
      lf_text(percentStr.c_str());
      lf_pop_font();
      lf_pop_style_props();
      
      lf_next_line();
      const vec2s progressBarSize = (vec2s){400, 6};

      LfUIElementProps props = lf_get_theme().slider_props;
      props.border_width = 0;
      props.color = GRAY;
      props.text_color = BLUE_GRAY;  
      props.corner_radius = 1.5f;
      props.margin_top = 0;
      props.margin_left = 15;
      props.margin_right = 0;
      lf_push_style_props(props);
      lf_progress_bar_int(downloadedFileCount, 0, (float)state.downloadPlaylistFileCount, progressBarSize.x, progressBarSize.y);
      lf_pop_style_props();
    }

    lf_next_line();

    {
      const float buttonSize = 180.0f;
      LfUIElementProps props = call_to_action_button_style();
      props.color = lf_color_brightness(LYSSA_BACKGROUND_COLOR, 1.5f);
      props.margin_left = 15;
      props.margin_right = 0;
      props.corner_radius = 8.0f;
      props.border_width = 1.0f;
      props.border_color = GRAY;
      props.margin_top = 10;
      props.text_color = LF_WHITE;
      lf_push_style_props(props);
      if(lf_button_fixed("Cancle", buttonSize, -1) == LF_CLICKED) {
        state.playlistDownloadRunning = false;
        system("pkill yt-dlp &");
      }
      lf_pop_style_props();
    }
  }
  lf_div_end();

  if(LyssaUtils::getCommandOutput("pgrep yt-dlp") == "") {
    beginBottomNavBar();
    backButtonTo(GuiTab::Dashboard, [&](){
        loadPlaylists();
        });
    renderTrackMenu();
    lf_div_end();
  }
}

void renderOnPlaylist() {
  Playlist& currentPlaylist = state.playlists[state.currentPlaylist];

  static bool clearedPlaylist = false;

  if(state.playlistDownloadRunning) {
    if(!clearedPlaylist) {
      currentPlaylist.musicFiles.clear();
      state.loadedPlaylistFilepaths.clear();
      state.playlistFileThumbnailData.clear();
      Playlist::save(state.currentPlaylist);
      clearedPlaylist = true;
    }


    static float ytdlpDownTimer = 0.0f;
    bool downloadFinished = false;
    if(LyssaUtils::getCommandOutput("pgrep yt-dlp") == "") {
      ytdlpDownTimer += state.deltaTime;
      if(ytdlpDownTimer >= 2.0f) {
        ytdlpDownTimer = 0.0f;
        downloadFinished = true;
      }
    } else {
      ytdlpDownTimer = 0.0f;
    }
    if(downloadFinished) {
      state.playlistDownloadRunning = false;
      state.loadedPlaylistFilepaths = PlaylistMetadata::getFilepaths(std::filesystem::directory_entry(LYSSA_DIR + "/downloaded_playlists/" + state.downloadingPlaylistName));
      for (const auto& entry : std::filesystem::directory_iterator(LYSSA_DIR + "/downloaded_playlists/" + state.downloadingPlaylistName)) {
        if (entry.is_regular_file() && 
            !Playlist::containsFile(entry.path().string(), state.currentPlaylist) && 
            SoundTagParser::isValidSoundFile(entry.path().string()) && entry.path().extension() == ".mp3") {
          state.loadedPlaylistFilepaths.emplace_back(entry.path().string());
          state.playlistFileFutures.emplace_back(std::async(std::launch::async, addFileToPlaylistAsync, 
                &state.playlists[state.currentPlaylist].musicFiles, entry.path().string(), state.currentPlaylist));
        }
      }
    }
  }

  // Playlist Heading
  {
    lf_push_font(&state.musicTitleFont);
    vec2s titleSize = lf_text_dimension(currentPlaylist.name.c_str());
    bool onTitleArea = lf_hovered(LF_PTR, (vec2s){(float)state.win->getWidth(), titleSize.y});
    // Title 
    {
      LfUIElementProps props = lf_get_theme().text_props;
      props.text_color = LYSSA_PLAYLIST_COLOR;
      props.margin_bottom = 20;
      lf_push_style_props(props);
      lf_set_cull_end_x(state.win->getWidth() - 250.0f);
      lf_text(currentPlaylist.name == "favourites" ? "Favourites" : currentPlaylist.name.c_str());
      lf_unset_cull_end_x();
      lf_pop_style_props();
    }
    lf_pop_font();
    LfUIElementProps props = lf_get_theme().button_props;
    props.color = LF_NO_COLOR;
    props.border_width = 0;
    props.margin_left = 10;
    props.margin_right = 0;
    props.padding = 0;
    props.margin_top = 0; 
    lf_push_style_props(props);
    lf_set_image_color(LF_WHITE);

    lf_unset_image_color();
    lf_pop_style_props();

    // "Add More" button
    if(!currentPlaylist.musicFiles.empty())
    {
      lf_push_font(&state.h5Font);
      const char* text = "Add more music";

      float textWidth = lf_text_dimension(text).x;

      LfUIElementProps props = primary_button_style();  
      props.margin_left = 0;
      props.margin_right = 0;

      // Rendering the button in the top right
      lf_set_ptr_x_absolute(state.win->getWidth() - (textWidth + props.padding * 2.0f) - DIV_START_X);

      lf_push_style_props(props);
      float ptr_x = lf_get_ptr_x();
      float ptr_y = lf_get_ptr_y();
      if(lf_button(text) == LF_CLICKED) {
        state.popups[PopupType::TwoChoicePopup] = std::make_unique<TwoChoicePopup>(
            400, 
            "How do you want to add Music?", 
            "From File", 
            "From Folder", 
            [&](){
            changeTabTo(GuiTab::PlaylistAddFromFile);
            state.popups[PopupType::TwoChoicePopup]->shouldRender = false;
            lf_div_ungrab();
            }, 
            [&](){
            if(state.playlistAddFromFolderTab.currentFolderPath.empty()) {
            state.playlistAddFromFolderTab.currentFolderPath = std::string(getenv(HOMEDIR));
            state.playlistAddFromFolderTab.folderContents = loadFolderContents(state.playlistAddFromFolderTab.currentFolderPath);
            }
            changeTabTo(GuiTab::PlaylistAddFromFolder);
            state.popups[PopupType::TwoChoicePopup]->shouldRender = false;
            lf_div_ungrab();
            }); 
        state.popups[PopupType::TwoChoicePopup]->shouldRender = !state.popups[PopupType::TwoChoicePopup]->shouldRender;
      }  
      lf_pop_style_props();
    }


    { 
      lf_next_line();
      if(renderMenuBarElement("Sync Downloads", state.icons["sync"].id)) {
        terminateAudio();
        system(std::string(LYSSA_DIR + "/scripts/download.sh \"" + currentPlaylist.url + "\" " + LYSSA_DIR + "/downloaded_playlists/ &").c_str());

        state.playlistDownloadRunning = true;
        state.downloadingPlaylistName = std::filesystem::path(currentPlaylist.path).filename().string();
        state.downloadPlaylistFileCount = LyssaUtils::getPlaylistFileCountURL(currentPlaylist.url);

        clearedPlaylist = false;
      }
      if(renderMenuBarElement("Search", state.icons["search"].id)) {
        state.searchPlaylistResults.clear();
        state.searchPlaylistResults = matchSoundFiles(state.playlists[state.currentPlaylist].musicFiles, "");
        changeTabTo(GuiTab::SearchPlaylist);
      }
      if(renderMenuBarElement("Jump to top", state.icons["jump_to_top"].id)) {
        float filePosY = currentPlaylist.musicFiles[0].renderPosY;
        currentPlaylist.scroll = -filePosY;
      }
      if(renderMenuBarElement("Jump to bottom", state.icons["jump_to_bottom"].id)) {
        float filePosY = currentPlaylist.musicFiles[currentPlaylist.musicFiles.size() - 1].renderPosY;
        currentPlaylist.scroll = -filePosY;
      }
      lf_set_ptr_y_absolute(lf_get_ptr_y() + 60.0f);
    }
  }

  if(state.playlistDownloadRunning) {
    lf_set_ptr_y(100);
    lf_push_font(&state.h5Font);
    const char* text = "Syncing playlist downloads...";
    float textWidth = lf_text_dimension(text).x;

    // Centering the text
    lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth - textWidth) / 2.0f - DIV_START_X);

    LfUIElementProps props = lf_get_theme().text_props;
    props.margin_top = 80;
    props.margin_left = 0;
    props.margin_right = 0;
    lf_push_style_props(props);
    lf_text(text);
    lf_pop_style_props();
    lf_pop_font();
    lf_next_line();
    {
        std::string downloadedPlaylistDir = LYSSA_DIR + "/downloaded_playlists/" + state.downloadingPlaylistName; 
        uint32_t downloadedFileCount = MIN(LyssaUtils::getLineCountFile(downloadedPlaylistDir + "/archive.txt"), state.downloadPlaylistFileCount);
        const vec2s progressBarSize = (vec2s){400, 6};

        LfUIElementProps props = lf_get_theme().slider_props;
        props.border_width = 0;
        props.color = GRAY;
        props.text_color = BLUE_GRAY;  
        props.corner_radius = 1.5f;
        props.margin_top = 15;
        props.margin_left = 0;
        props.margin_right = 0;

        {
            vec2s textDim = lf_text_dimension(std::to_string(downloadedFileCount).c_str());

            lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth - progressBarSize.x - textDim.x * 2.0f) / 2.0f - DIV_START_X);
            LfUIElementProps props = lf_get_theme().text_props;
            props.color = lf_color_brightness(GRAY, 1.5);
            props.margin_top = 15 - (textDim.y - progressBarSize.y) / 2.0f;

            lf_push_style_props(props);
            lf_push_font(&state.h6Font);
            lf_text(std::to_string(downloadedFileCount).c_str());
            lf_pop_style_props();
            lf_pop_font();
        }

        lf_push_style_props(props);
        lf_progress_bar_int(downloadedFileCount, 0, (float)state.downloadPlaylistFileCount, progressBarSize.x, progressBarSize.y);
        lf_pop_style_props();

        {
            std::string totalFileCount = std::to_string(state.downloadPlaylistFileCount);
            static float totalFileCountHeight = lf_text_dimension(totalFileCount.c_str()).y;

            LfUIElementProps props = lf_get_theme().text_props;
            props.color = lf_color_brightness(GRAY, 1.5);
            props.margin_top = 15 - (totalFileCountHeight - progressBarSize.y) / 2.0f;

            lf_push_style_props(props);
            lf_push_font(&state.h6Font);
            lf_text(totalFileCount.c_str());
            lf_pop_style_props();
            lf_pop_font();
        }
    }
  } else if(currentPlaylist.musicFiles.empty()) {
      lf_next_line();
    // Text
    if(state.currentPlaylist != 0) // 0th playlist is favourites
    {
      lf_set_ptr_y(100);
      lf_push_font(&state.h5Font);
      const char* text = "There is no music in this playlist.";
      float textWidth = lf_text_dimension(text).x;

      // Centering the text
      lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth - textWidth) / 2.0f - DIV_START_X);

      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_top = 80;
      props.margin_left = 0;
      props.margin_right = 0;
      lf_push_style_props(props);
      lf_text(text);
      lf_pop_style_props();
      lf_pop_font();
      lf_next_line();
      // Buttons 
      {
          lf_push_font(&state.h6Font);
          const float buttonWidth = 175;
          LfUIElementProps props = lf_get_theme().button_props;
          props.color = (LfColor){240, 240, 240, 255};
          props.text_color = LF_BLACK;
          props.corner_radius = 10; 
          props.border_width = 0;
          props.margin_top = 20;

          // Centering the buttons
          lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth - 
                      (buttonWidth + props.padding * 2.0f) * 2.0f - (props.margin_right + props.margin_left) * 2.f) / 2.0f - DIV_START_X);

          lf_push_style_props(props);
          if(lf_button_fixed("Add from file", buttonWidth, 40) == LF_CLICKED) {
              changeTabTo(GuiTab::PlaylistAddFromFile);
          }
          if(lf_button_fixed("Add from Folder", buttonWidth, 40) == LF_CLICKED) {
              if(state.playlistAddFromFolderTab.currentFolderPath.empty()) {
                  state.playlistAddFromFolderTab.currentFolderPath = std::string(getenv(HOMEDIR));
                  state.playlistAddFromFolderTab.folderContents =  loadFolderContents(state.playlistAddFromFolderTab.currentFolderPath);
              }
              if(state.soundHandler.isInit) {
                  state.soundHandler.stop();
                  state.soundHandler.uninit();
                  state.currentSoundFile = nullptr;
              }
              changeTabTo(GuiTab::PlaylistAddFromFolder);
          }
          lf_pop_style_props();
          lf_pop_font();
      }
    } else { // On favourites
      lf_set_ptr_y(100);
      lf_push_font(&state.h4Font);
      const char* text = "Your favourites appear here";
      float textWidth = lf_text_dimension(text).x;

      // Centering the text
      lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth - textWidth) / 2.0f - DIV_START_X);

      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_top = 80;
      props.margin_left = 0;
      props.margin_right = 0;
      lf_push_style_props(props);
      lf_text(text);
      lf_pop_style_props();
      lf_pop_font();
      lf_next_line();

      const char* subtext = "You can add favourites from other playlists.";
      lf_push_font(&state.h6Font);
      float subtextWidth = lf_text_dimension(subtext).x;
      lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth - subtextWidth) / 2.0f - DIV_START_X);
      props.margin_top = 10;
      props.text_color = lf_color_brightness(GRAY, 1.2f);
      lf_push_style_props(props);
      lf_text(subtext);
      lf_pop_font();
      lf_pop_style_props();
      lf_pop_font();
      lf_next_line();
    }
  } else {
    lf_next_line();


    /* Heading */
    {
      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_bottom = 20;
      props.margin_right = 30;
      props.text_color = lf_color_brightness(GRAY, 1.6);
      lf_push_style_props(props);
      lf_text("#");
      lf_pop_style_props();

      props.margin_left = 0;
      props.margin_right = 0;
      lf_push_style_props(props);
      lf_text("Title");


      lf_set_ptr_x_absolute((state.win->getWidth()) / 1.5f + 100);
      lf_text("Year");

      lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth) - (lf_text_dimension("Duration").x) -  DIV_START_X * 2 - lf_get_theme().text_props.margin_left);
      lf_text("Duration");
      lf_pop_style_props();

      lf_next_line();
    }
    // Seperator
    {
      LfUIElementProps props = lf_get_theme().button_props;
      props.color = lf_color_brightness(GRAY, 0.4);
      lf_push_style_props(props);
      lf_seperator();
      lf_pop_style_props();
    }

    // Begin a new div container for the files
    lf_div_begin_ex(LF_PTR, ((vec2s){(float)state.win->getWidth() - DIV_START_X * 1.5f - state.sideNavigationWidth, 
          (float)state.win->getHeight() - DIV_START_Y * 2 - lf_get_ptr_y() - 
          (BACK_BUTTON_HEIGHT + BACK_BUTTON_MARGIN_BOTTOM)}), true, &currentPlaylist.scroll, 
        &currentPlaylist.scrollVelocity);

    lf_next_line();

    static bool draggingTrack = false;
    static std::string draggingTrackTitle = "";
    static int32_t draggingTrackIndex = -1;
    for(uint32_t i = 0; i < currentPlaylist.musicFiles.size(); i++) {
      SoundFile& file = currentPlaylist.musicFiles[i];
      bool onActionButton = false;
      {
        vec2s thumbnailContainerSize = PLAYLIST_FILE_THUMBNAIL_SIZE;
        vec2s startPtr= LF_PTR;

        float marginBottomThumbnail = 10.0f, marginTopThumbnail = 5.0f;

        file.renderPosY = (lf_get_ptr_y() - currentPlaylist.scroll) - lf_get_current_div().aabb.size.y;

        LfAABB fileAABB = (LfAABB){
          .pos = (vec2s){lf_get_ptr_x(), lf_get_ptr_y()},
            .size = (vec2s){(float)state.win->getWidth() - DIV_START_X * 2, (float)thumbnailContainerSize.y + marginBottomThumbnail}
        };

        bool hoveredTextDiv = lf_hovered(fileAABB.pos, fileAABB.size);
        if(hoveredTextDiv && lf_mouse_move_event().happened) {
          currentPlaylist.selectedFile = (int32_t)i;
        }
        if(hoveredTextDiv && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_RIGHT)) {
          state.popups[PopupType::PlaylistFileDialoguePopup] = std::make_unique<PlaylistFileDialoguePopup>(file.path, 
                  (vec2s){(float)lf_get_mouse_x() + 10, (float)lf_get_mouse_y() + 10});
          state.popups[PopupType::PlaylistFileDialoguePopup]->shouldRender = true;
        }
        if(currentPlaylist.playingFile == i) {
          lf_rect_render(fileAABB.pos, fileAABB.size, lf_color_brightness(GRAY, 0.75),
              LF_NO_COLOR, 0.0f, 3.0f);
        } 

        {
          LfUIElementProps props = lf_get_theme().button_props;
          props.color = LF_NO_COLOR; props.border_width = 0.0f;
          props.padding = 10.0f;
          props.margin_top = 10;
          lf_push_style_props(props);
          uint32_t id = state.icons["move"].id;
          LfClickableItemState state = lf_image_button(((LfTexture){.id = id, .width = 25, .height = 7}));

          if(lf_mouse_button_went_down(GLFW_MOUSE_BUTTON_LEFT) && state != LF_IDLE) {
            draggingTrackTitle = file.title;
            draggingTrackIndex = i;
          } 
          if(!draggingTrack && draggingTrackTitle != "" && (fabsf(lf_get_mouse_x_delta()) > 2 || fabsf(lf_get_mouse_y_delta()) > 2)) {
            draggingTrack = true;
          }
          lf_pop_style_props();
          
        }
        // Index 
        {
          std::stringstream indexSS;
          indexSS << i + 1;
          std::string indexStr = indexSS.str();
          vec2s indexPos = (vec2s){lf_get_ptr_x() + 10, lf_get_ptr_y() + (thumbnailContainerSize.y - lf_text_dimension(indexStr.c_str()).y) / 2.0f};
          if(hoveredTextDiv || (i == currentPlaylist.playingFile)) {
            bool hoveredPlayButton = lf_hovered((vec2s){indexPos.x - 5, indexPos.y}, 
                (vec2s){(float)lf_get_theme().font.font_size, (float)lf_get_theme().font.font_size}); 
            onActionButton = hoveredPlayButton;
            if(hoveredTextDiv && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT) && onActionButton && state.playlistFileFutures.empty()) {
              if(currentPlaylist.playingFile == i) {
                if(state.soundHandler.isPlaying)
                  state.soundHandler.stop();
                else 
                  state.soundHandler.play();
              } else {
                playlistPlayFileWithIndex(i, state.currentPlaylist);
                state.currentSoundFile = &file;
              }
            }
            lf_image_render((vec2s){indexPos.x - 2.5f, indexPos.y}, LF_WHITE, 
                (LfTexture){.id = (i == currentPlaylist.playingFile && state.soundHandler.isPlaying) ? 
                state.icons["pause_raw"].id : state.icons["play_raw"].id, .width = lf_get_theme().font.font_size, .height = lf_get_theme().font.font_size}, 
                LF_NO_COLOR, 0.0f, 0.0f);
          } else {
            renderTextRaw(indexPos, indexStr.c_str(), lf_get_theme().font, LF_WHITE, -1);
          }
          lf_set_ptr_x_absolute(lf_get_ptr_x() + 50);
        }

        // Thumbnail + Title
        {
          lf_set_ptr_y_absolute(lf_get_ptr_y() + marginTopThumbnail);
          LfClickableItemState thumbnailState = renderSoundFileThumbnail(thumbnailContainerSize, file, nullptr, false);

          if(thumbnailState == LF_CLICKED && i != currentPlaylist.playingFile) {
            state.currentSoundFile = &file;
            if(state.onTrackTab.trackThumbnail.width != 0) {
              lf_free_texture(&state.onTrackTab.trackThumbnail);
            }
            state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path, (vec2s){-1, -1});
            changeTabTo(GuiTab::OnTrack);
            playlistPlayFileWithIndex(i, state.currentPlaylist);
          } else if(thumbnailState == LF_CLICKED && i == currentPlaylist.playingFile) {
            if(state.onTrackTab.trackThumbnail.width != 0) {
              lf_free_texture(&state.onTrackTab.trackThumbnail);
            }
            state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path, (vec2s){-1, -1});
            changeTabTo(GuiTab::OnTrack);
          }
         
          if(!onActionButton) {
            onActionButton = (thumbnailState != LF_IDLE); 
          }
          lf_set_line_height(thumbnailContainerSize.y + marginBottomThumbnail);

          std::filesystem::path fsPath(file.path);
          std::string filename = file.title == "" ? removeFileExtensionW(fsPath.filename().string()) : file.title;


          /* Title */
          lf_set_ptr_x_absolute(lf_get_ptr_x() + 10.0f);

          lf_set_cull_end_x(((state.win->getWidth()) / 1.5f + 100) - 50);

          renderTextRaw((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + marginTopThumbnail}, filename.c_str(), state.h6BoldFont, 
              (currentPlaylist.selectedFile == i ? lf_color_brightness(LF_WHITE, 0.7f) : LF_WHITE), -1);
          renderTextRaw((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + marginTopThumbnail + state.h6Font.font_size}, file.artist.empty() ? "-" : file.artist.c_str(), state.h5Font,
              lf_color_brightness(GRAY, 1.4f));

          lf_unset_cull_end_x();
        }

        // Year
        {
          lf_set_ptr_x_absolute((state.win->getWidth()) / 1.5f + 100);
          LfUIElementProps props = lf_get_theme().text_props;
          props.text_color = lf_color_brightness(GRAY, 1.6f);
          props.margin_left = 0;
          props.margin_right = 0;
          props.margin_top = (thumbnailContainerSize.y - lf_text_dimension(std::to_string(file.releaseYear).c_str()).y) / 2.0f;
          lf_push_style_props(props);
          if(file.releaseYear != 0)
            lf_text(std::to_string(file.releaseYear).c_str());
          else 
            lf_text("-");
          lf_pop_style_props();
          // 0th playlist index is favourites
          if((Playlist::containsFile(file.path, 0) || Playlist::metadataContainsFile(file.path, 0)) && state.currentPlaylist != 0)
          {
              LfUIElementProps props = lf_get_theme().button_props;
              props.border_width = 0.0f;
              props.corner_radius = 5.0f;
              props.margin_left = 15.0f;
              props.margin_top = 12.5f;
              props.padding = 0;
              props.color = LF_NO_COLOR;
              lf_set_image_color(LYSSA_PLAYLIST_COLOR);
              lf_push_style_props(props);
              lf_image_button(((LfTexture){.id = state.icons["favourite_selected"].id, .width = 20, .height = 20}));
              lf_pop_style_props();
              lf_unset_image_color();
          }
        }

        if(lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT) && hoveredTextDiv && !onActionButton && state.playlistFileFutures.empty()) {
          if(!draggingTrack) {
            playlistPlayFileWithIndex(i, state.currentPlaylist);
            state.currentSoundFile = &file;
          } else {
            if(state.currentSoundFile) {
              if(*state.currentSoundFile == currentPlaylist.musicFiles[draggingTrackIndex]) {
                terminateAudio();
              }
            }
            moveFileInPlaylistIdx(state.currentPlaylist, draggingTrackIndex, i);
            draggingTrack = false;
            draggingTrackTitle = "";
          }
        }

        // Duration 
        {
          lf_set_ptr_x((state.win->getWidth() - state.sideNavigationWidth) - (lf_text_dimension("Duration").x) -  DIV_START_X * 2 - lf_get_theme().text_props.margin_left);
          LfUIElementProps props = lf_get_theme().text_props;
          std::string durationText = formatDurationToMins(file.duration);
          props.margin_top = (thumbnailContainerSize.y - lf_text_dimension(durationText.c_str()).y) / 2.0f;
          lf_push_style_props(props);
          LfClickableItemState durationState = lf_button(durationText.c_str());
          if(lf_mouse_button_went_down(GLFW_MOUSE_BUTTON_LEFT) && durationState != LF_IDLE) {
            draggingTrackTitle = file.title;
            draggingTrackIndex = i;
          } 
          if(!draggingTrack && draggingTrackTitle != "" && (fabsf(lf_get_mouse_x_delta()) > 2 || fabsf(lf_get_mouse_y_delta()) > 2)) {
            draggingTrack = true;
          }
          lf_pop_style_props();
        }
        if(draggingTrack && hoveredTextDiv) {
          lf_rect_render(startPtr, (vec2s){fileAABB.size.x - state.sideNavigationWidth - DIV_START_X * 2.0f , 1}, LYSSA_PLAYLIST_COLOR, LF_NO_COLOR, 0.0f, 0.0f);
        }
        if(lf_get_current_div().id != lf_get_selected_div().id && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
          draggingTrack = false;
          draggingTrackTitle = "";
        }
        lf_next_line(); 
      }
    }
    lf_div_end();
    if(draggingTrack) {
      LfUIElementProps props = lf_get_theme().div_props;
      props.color = GRAY;
      props.corner_radius = 3.5f;
      lf_push_style_props(props);
      lf_div_begin(((vec2s){(float)lf_get_mouse_x() + 10, (float)lf_get_mouse_y() + 10}), ((vec2s){
            lf_text_dimension(draggingTrackTitle.c_str()).x + 15.0f, 50.0f}), false);
      lf_pop_style_props();

      lf_text(draggingTrackTitle.c_str());

      lf_div_end();
    }
  }

  if(state.playlistFileFutures.empty()) {
    beginBottomNavBar();
    backButtonTo(GuiTab::Dashboard, [&](){
        if(state.dashboardTab == DashboardTab::Favourites) {
          state.dashboardTab = DashboardTab::Home;
        }
        loadPlaylists();
        });
    renderTrackMenu();
    lf_div_end();
  }
}
void renderOnTrack() {
  if(!state.currentSoundFile) return;
  SoundFile* soundFile = state.currentSoundFile;

  int32_t winWidth = state.win->getWidth();
  int32_t winHeight = state.win->getHeight();
  int32_t containerSize = MAX(((winWidth < winHeight) ? winWidth / 2 : winHeight / 2), 400);

  float margin = 15;
  float controlsSpaceHeight = 50; 

  vec2s containerPos = (vec2s){
    .x = (winWidth - containerSize) / 2.0f,
    .y = ((winHeight - (DIV_START_Y + BACK_BUTTON_HEIGHT * 2.0f)) - 
        (containerSize + margin + lf_get_theme().font.font_size * 2.0f + margin + controlsSpaceHeight)) / 2.0f
  };

  lf_set_ptr_x_absolute(containerPos.x);
  lf_set_ptr_y_absolute(containerPos.y); 

  // Thumbnail container
  lf_rect_render(LF_PTR, (vec2s){(float)containerSize, (float)containerSize}, lf_color_brightness(GRAY, 0.5), LF_NO_COLOR, 0.0f, 
          PLAYLIST_ON_TRACK_CORNER_RADIUS);

  // Fullscreen button 
  if(lf_hovered(LF_PTR, (vec2s){(float)containerSize, (float)containerSize}))
  {
    const uint32_t buttonSize = 24; 
    const float padding = 10.0f;
    const float add = containerSize - buttonSize - padding;

    vec2s buttonPos = (vec2s){lf_get_ptr_x() + add, lf_get_ptr_y() + add};
    bool hoveredButton = lf_hovered(buttonPos, (vec2s){(float)buttonSize, (float)buttonSize});
    uint32_t icon = hoveredButton ? state.icons["fullscreen-full"].id : state.icons["fullscreen"].id; 

    if(hoveredButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      changeTabTo(GuiTab::TrackFullscreen);
    }

    lf_image_render(buttonPos, LF_WHITE, (LfTexture){.id = icon, .width = buttonSize, .height = buttonSize}, LF_NO_COLOR, 0.0f, 0.0f);
  }
  // Thumbnail
  {
    LfTexture thumbnail = state.onTrackTab.trackThumbnail.width == 0 ? state.icons["music_note"] : state.onTrackTab.trackThumbnail;
    float thumbnailAspect = (float)thumbnail.width / (float)thumbnail.height;
    float containerAspect = (float)containerSize / (float)containerSize;
    float scaleFactor;
    if (thumbnailAspect > containerAspect) {
        scaleFactor = (float)containerSize / (float)thumbnail.width;
    } else {
        scaleFactor = (float)containerSize / (float)thumbnail.height;
    }
    float thumbnailWidth = thumbnail.width * scaleFactor; 
    float thumbnailHeight = thumbnail.height * scaleFactor; 

    lf_image_render((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (containerSize - thumbnailHeight) / 2.0f}, 
        LF_WHITE, (LfTexture){.id = thumbnail.id, .width = (uint32_t)thumbnailWidth, .height = (uint32_t)thumbnailHeight}, 
        LF_NO_COLOR, 0.0f, 
        (thumbnailHeight >= containerSize - 1) ? PLAYLIST_ON_TRACK_CORNER_RADIUS : 0.0f);

    lf_set_ptr_y_absolute(lf_get_ptr_y() + containerSize + margin);
  }

  // Title
  {
    std::string filename = soundFile->title == "" ? 
      removeFileExtensionW(soundFile->path.filename().string()) : soundFile->title;

    float textWidth = lf_text_dimension(filename.c_str()).x; 
    if(textWidth > containerSize) {
      lf_set_ptr_x_absolute(containerPos.x);
      lf_set_cull_end_x(containerPos.x + containerSize);
    } else {
      lf_set_ptr_x_absolute((winWidth - textWidth) / 2.0f);
    }
    renderTextRaw(LF_PTR, filename.c_str(), lf_get_theme().font, LF_WHITE);
    lf_unset_cull_end_x();
  }

  // Advance to the next line
  lf_set_ptr_y_absolute(lf_get_ptr_y() + lf_get_theme().font.font_size);

  // Arist
  {
    std::string artist = soundFile->artist == "" ? "-" : soundFile->artist;

    float textWidth = lf_text_dimension(artist.c_str()).x; 
    lf_set_ptr_x_absolute((winWidth - textWidth) / 2.0f);
    renderTextRaw(LF_PTR, artist.c_str(), lf_get_theme().font, lf_color_brightness(GRAY, 1.5f));
  }

  lf_set_ptr_y_absolute(lf_get_ptr_y() + lf_get_theme().font.font_size + margin);

  float progressBarHeight = 5.0f;
  // Progress position in seconds
  {
    lf_set_ptr_x_absolute((winWidth - containerSize) / 2.0f);
    lf_push_font(&state.h6Font);
    std::string durationMins = formatDurationToMins(state.soundHandler.getPositionInSeconds());
    LfUIElementProps props = lf_get_theme().text_props;
    props.margin_top = 15.0f;
    props.margin_left = 0.0f;
    props.margin_right = 5.0f;
    lf_push_style_props(props);
    lf_text(durationMins.c_str());
    lf_pop_style_props();
    lf_pop_font();
  }
  // Total sound duration
  {
    lf_push_font(&state.h6Font);
    LfUIElementProps props = lf_get_theme().text_props;
    std::string durationMins = formatDurationToMins(state.soundHandler.lengthInSeconds); 
    lf_set_ptr_x_absolute(((winWidth - containerSize) / 2.0f) + containerSize);
    props.margin_left = -lf_text_dimension(durationMins.c_str()).x;
    props.margin_right = 0.0f;
    props.margin_top = 15.0f;
    lf_push_style_props(props);
    lf_text(durationMins.c_str());
    lf_pop_style_props();
    lf_pop_font();
  }
  // Progress Bar
  {
    LfUIElementProps props = lf_get_theme().slider_props;
    props.margin_top = 0.0f;
    props.color = lf_color_brightness(GRAY, 1.5f);
    props.text_color = LF_WHITE;
    props.corner_radius = 1.5f;
    props.border_width = 0.0f;

    lf_push_style_props(props);
    lf_set_ptr_x_absolute((winWidth - containerSize) / 2.0f);

    state.trackProgressSlider.width = containerSize;
    state.trackProgressSlider.min = 0.0f;
    state.trackProgressSlider.max = state.soundHandler.lengthInSeconds;
    state.trackProgressSlider.height = progressBarHeight;
    state.trackProgressSlider.handle_size = progressBarHeight * 3.0f;


    vec2s posPtr = (vec2s){lf_get_ptr_x() + props.margin_left, lf_get_ptr_y() + props.margin_top};

    LfClickableItemState progressBar = lf_slider_int(&state.trackProgressSlider);

    lf_rect_render(posPtr, 
        (vec2s){(float)state.trackProgressSlider.handle_pos, (float)state.trackProgressSlider.height}, 
        props.text_color, LF_NO_COLOR, 0.0f, props.corner_radius);


    if(progressBar == LF_CLICKED) {
      state.soundHandler.setPositionInSeconds(state.currentSoundPos);
    }

    lf_pop_style_props();
  }
  lf_set_ptr_y_absolute(lf_get_ptr_y() + progressBarHeight + margin);

  // Controls
  {
    float controlSize = 35;
    float controlMargin = margin * 1.5;

    float controlsSpaceWidth = 
      controlSize + controlMargin + 
      controlsSpaceHeight + controlMargin + 
      controlSize;

    lf_set_ptr_x_absolute((winWidth - controlsSpaceWidth) / 2.0f);

    /*
    bool onShuffleButton = lf_hovered((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f}, 
        (vec2s){controlSize, controlSize});

    if(onShuffleButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      state.shuffle = !state.shuffle;
    }

    lf_set_image_color(lf_color_brightness(GRAY, 2.5f));

    lf_image_render((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f},
        LF_WHITE, (LfTexture){.id = state.icons[state.shuffle ? "shuffle_active" : "shuffle"].id, 
        .width = (uint32_t)controlSize, .height = (uint32_t)controlSize}, LF_NO_COLOR, 0.0f, 0.0f);

    lf_set_ptr_x_absolute(lf_get_ptr_x() + controlSize + controlMargin);
    */
    // Skip Down
    bool onSkipDownButton = lf_hovered((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f}, 
        (vec2s){controlSize, controlSize});

    if(onSkipDownButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      skipSoundDown(state.playingPlaylist);
    }


    lf_image_render((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f},
        lf_color_brightness(GRAY, 1.5f), (LfTexture){.id = state.icons["skip_down"].id, 
        .width = (uint32_t)(controlSize), .height = (uint32_t)controlSize}, LF_NO_COLOR, 0.0f, 0.0f);

    lf_set_ptr_x_absolute(lf_get_ptr_x() + controlSize + controlMargin);

    // Play/Pause
    bool onPlayButton = lf_hovered(LF_PTR, (vec2s){controlsSpaceHeight, controlsSpaceHeight});
    if(onPlayButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      if(state.soundHandler.isPlaying)
        state.soundHandler.stop();
      else 
        state.soundHandler.play();
    }
    lf_image_render(LF_PTR, LF_WHITE, (LfTexture){.id = state.icons[state.soundHandler.isPlaying ? "pause" : "play"].id, .width = (uint32_t)(controlsSpaceHeight), 
        .height = (uint32_t)controlsSpaceHeight}, LF_NO_COLOR, 0.0f, 0.0f);

    lf_set_ptr_x_absolute(lf_get_ptr_x() + controlsSpaceHeight + controlMargin);


    // Skip Up
    bool onSkipUpButton = lf_hovered((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f},
        (vec2s){controlSize, controlSize});
    if(onSkipUpButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      skipSoundUp(state.playingPlaylist);
    }

    lf_image_render((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f}, lf_color_brightness(GRAY, 1.5f), (LfTexture){.id = state.icons["skip_up"].id, 
        .width = (uint32_t)(controlSize), .height = (uint32_t)controlSize}, LF_NO_COLOR, 0.0f, 0.0f);

    /*
    bool onReplayButton = lf_hovered((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f}, 
        (vec2s){controlSize, controlSize});

    if(onReplayButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      state.replayTrack = !state.replayTrack;
    }

    lf_image_render((vec2s){lf_get_ptr_x(), lf_get_ptr_y() + (controlsSpaceHeight - controlSize) / 2.0f},
        LF_WHITE, (LfTexture){.id = state.icons[state.replayTrack ? "replay_active" : "replay"].id, 
        .width = (uint32_t)(controlSize), .height = (uint32_t)controlSize}, LF_NO_COLOR, 0.0f, 0.0f);

    lf_unset_image_color();
    */

    vec2s shufflePos = (vec2s){state.win->getWidth() - controlSize - DIV_START_X - BACK_BUTTON_WIDTH, 
        state.win->getHeight() - controlSize - DIV_START_Y - BACK_BUTTON_HEIGHT - 5};

    bool onShuffleButton = lf_area_hovered(shufflePos, (vec2s){controlSize, controlSize});

    if(onShuffleButton && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      state.shuffle = !state.shuffle;
    }

    if(!state.shuffle) {
      lf_set_image_color(lf_color_brightness(GRAY, 2.5f));
    } else {
      lf_set_image_color(lf_color_brightness(LF_WHITE, 0.8));
    }

    lf_image_render(shufflePos, !state.shuffle ? LF_WHITE : LF_BLACK, 
        (LfTexture){.id = state.icons[state.shuffle ? "shuffle_active" : "shuffle"].id, 
        .width = (uint32_t)controlSize, .height = (uint32_t)controlSize}, LF_NO_COLOR, 0.0f, 0.0f);
    lf_unset_image_color();
  }
  beginBottomNavBar();
  backButtonTo(state.previousTab, [&](){
      if(state.onTrackTab.trackThumbnail.width != 0)
      lf_free_texture(&state.onTrackTab.trackThumbnail);
      });   
  renderTrackMenu();
  lf_div_end();
}

void renderTrackFullscreen() {
  vec2s winSize =  {(float)state.win->getWidth(), (float)state.win->getHeight()};
  lf_div_begin(((vec2s){0.0f, 0.0f}), winSize, false);
  vec2s containerSize = winSize;

  lf_rect_render((vec2s){0.0f, 0.0f}, (vec2s){(float)containerSize.x, (float)containerSize.y}, LF_BLACK, LF_NO_COLOR, 0.0f, 0.0f);

  LfTexture thumbnail = state.onTrackTab.trackThumbnail.width == 0 ? state.icons["music_note"] : state.onTrackTab.trackThumbnail;
  float thumbnailAspect = (float)thumbnail.width / (float)thumbnail.height;
  float containerAspect = (float)containerSize.x / (float)containerSize.y;
  float scaleFactor;
  if (thumbnailAspect > containerAspect) {
    scaleFactor = (float)containerSize.x / (float)thumbnail.width;
  } else {
    scaleFactor = (float)containerSize.y / (float)thumbnail.height;
  }
  float thumbnailWidth = thumbnail.width * scaleFactor; 
  float thumbnailHeight = thumbnail.height * scaleFactor; 

  // Thumbnail
  lf_image_render((vec2s){(containerSize.x - thumbnailWidth) / 2.0f, (containerSize.y - thumbnailHeight) / 2.0f}, 
      LF_WHITE, (LfTexture){.id = thumbnail.id, .width = (uint32_t)thumbnailWidth, .height = (uint32_t)thumbnailHeight}, 
      LF_NO_COLOR, 0.0f, 0.0f);


  if(state.trackFullscreenTab.showUI) {
    renderTextRaw((vec2s){DIV_START_X, DIV_START_Y}, state.currentSoundFile->title.c_str(), lf_get_theme().font, LF_WHITE);
    lf_div_end();
    lf_div_begin(((vec2s){DIV_START_X, state.win->getHeight() - BACK_BUTTON_HEIGHT - 45 - DIV_START_Y * 2}), ((vec2s){(float)state.win->getWidth(), BACK_BUTTON_HEIGHT + 45 + DIV_START_Y * 2}),
        false);
    backButtonTo(GuiTab::OnTrack);
    renderTrackVolumeControl();
    renderTrackProgress(true);
    lf_div_end();
  }
}
void renderPlaylistAddFromFile() {
  // Heading
  {
    lf_push_font(&state.h1Font);
    std::string text = "Add File to " + state.playlists[state.currentPlaylist].name;
    lf_text(text.c_str());
    lf_pop_font();
  }

  // Form Input 
  {
    lf_next_line();
    LfUIElementProps props = input_field_style();
    lf_push_style_props(props);
    lf_input_text(&state.playlistAddFromFileTab.pathInput.input);
    lf_pop_style_props();
    lf_next_line();
  }
  // Add Button 
  {
    lf_next_line();
    lf_push_style_props(call_to_action_button_style());
    if(lf_button_fixed("Add", 90, -1) == LF_CLICKED) {
      state.playlistAddFromFileTab.addFileStatus = Playlist::addFile(state.playlistAddFromFileTab.pathInput.input.buf, state.currentPlaylist);
      memset(state.playlistAddFromFileTab.pathInput.input.buf, 0, INPUT_BUFFER_SIZE);
      memset(state.playlistAddFromFileTab.pathInput.buffer, 0, INPUT_BUFFER_SIZE);
      state.playlistAddFromFileTab.addFileMessageTimer = 0.0f;
    }
    lf_pop_style_props();
  } 

  // File Status Message
  if(state.playlistAddFromFileTab.addFileStatus != FileStatus::None) {
    if(state.playlistAddFromFileTab.addFileMessageTimer < state.playlistAddFromFileTab.addFileMessageShowTime) {
      state.playlistAddFromFileTab.addFileMessageTimer += state.deltaTime;
      lf_next_line();
      lf_push_font(&state.h4Font);
      LfUIElementProps props = lf_get_theme().button_props;
      switch(state.playlistAddFromFileTab.addFileStatus) {
        case FileStatus::Failed:
          props.text_color = LYSSA_RED;
          lf_push_style_props(props);
          lf_text("Failed to add file to playlist.");
          lf_pop_style_props();
          break;
        case FileStatus::AlreadyExists:
          props.text_color = LYSSA_RED;
          lf_push_style_props(props);
          lf_text("File already exists in playlist.");
          lf_pop_style_props();
          break;
        case FileStatus::Success:
          props.text_color = LYSSA_GREEN;
          lf_push_style_props(props);
          lf_text("Added file to playlist.");
          lf_pop_style_props();
          break;
        default:
          break;
      }
      lf_pop_font();
    }
  }
  beginBottomNavBar();
  backButtonTo(state.previousTab);
  renderTrackMenu();
  lf_div_end();
}

static void renderTopBarAddFromFolder() {
  PlaylistAddFromFolderTab& tab = state.playlistAddFromFolderTab;
  LfUIElementProps props = lf_get_theme().text_props;
  const LfColor barColor = lf_color_brightness(GRAY, 0.5);

  props.color = barColor;
  props.corner_radius = 4.0f;
  props.padding = 12.0f;
  props.border_width = 0.0f;
  props.margin_right = 0.0f;
  lf_push_style_props(props);
  lf_text(state.playlistAddFromFolderTab.currentFolderPath.c_str());
  lf_pop_style_props();

  props = secondary_button_style();
  props.padding = 12.0f;
  props.margin_left = 0.0f;
  props.margin_right = 0;
  lf_push_style_props(props);
  LfClickableItemState addAllButton = lf_button("Add All");
  if(addAllButton == LF_CLICKED) {
    if(!state.playlistFileThumbnailData.empty()) {
      state.playlistFileThumbnailData.clear();
    }
    state.playlistAddFromFolderTab.addedFile = true;
    Playlist& currentPlaylist = state.playlists[state.currentPlaylist];
    std::ofstream metadata(currentPlaylist.path.string() + "/.metadata", std::ios::app);
    metadata.seekp(0, std::ios::end);
    for(const auto& entry : tab.folderContents) {
      if(!entry.is_directory() && 
          !Playlist::metadataContainsFile(entry.path().string(), state.currentPlaylist) && 
          SoundTagParser::isValidSoundFile(entry.path().string())) {
        metadata << "\"" << entry.path().string() << "\" ";
        state.loadedPlaylistFilepaths.push_back(entry.path().string());
      }
    }
    metadata.close();
  }
  lf_pop_style_props();
}
void renderPlaylistAddFromFolder() {
  {
    lf_push_font(&state.h1Font);
    LfUIElementProps props = lf_get_theme().text_props;
    props.margin_bottom = 15;
    lf_push_style_props(props);
    lf_text("Add files from folder");
    lf_pop_style_props();
    lf_pop_font();
  }

  lf_next_line();
  const std::vector<std::string> supportedFileFormats = 
    {".wav", ".flac", ".mp3", ".ogg", ".aac", ".aiff", ".opus", ".mod", ".midi", ".pcm"};


  renderFileDialogue(
      [&](std::filesystem::directory_entry entry){
      if(entry.is_directory()) {
      PlaylistAddFromFolderTab& tab = state.playlistAddFromFolderTab;
      tab.currentFolderPath = entry.path().string();
      tab.folderContents.clear();
      tab.folderContents = loadFolderContents(state.playlistAddFromFolderTab.currentFolderPath);
      lf_set_current_div_scroll(0.0f);
      lf_set_current_div_scroll_velocity(0.0f);
      }
      },
      [&](){
      PlaylistAddFromFolderTab& tab = state.playlistAddFromFolderTab;
      tab.currentFolderPath = std::filesystem::path(tab.currentFolderPath).parent_path().string();
      tab.folderContents.clear();
      tab.folderContents = loadFolderContents(state.playlistAddFromFolderTab.currentFolderPath);
      },
      renderTopBarAddFromFolder,
      [&](std::filesystem::directory_entry entry, bool hovered){
      (void)hovered;
      LfUIElementProps props = lf_get_theme().button_props;
      props.margin_top = 0.0f;
      props.color = LF_NO_COLOR;
      props.padding = 2.5f;
      props.border_width = 0.0f;
      lf_set_image_color((Playlist::metadataContainsFile(entry.path().string(), state.currentPlaylist) && !entry.is_directory()) ? LYSSA_GREEN : LF_WHITE);
      lf_push_style_props(props);
      const vec2s iconSize = (vec2s){25, 25};
      LfTexture icon = (LfTexture){
        .id = (entry.is_directory()) ? state.icons["folder"].id : state.icons["file"].id, 
          .width = (uint32_t)iconSize.x, 
          .height = (uint32_t)iconSize.y
      };
      if(lf_image_button(icon) == LF_CLICKED && !entry.is_directory() && 
          !Playlist::metadataContainsFile(entry.path().string(), state.currentPlaylist) && 
          SoundTagParser::isValidSoundFile(entry.path().string())) {
        std::ofstream metadata(state.playlists[state.currentPlaylist].path.string() + "/.metadata", std::ios::app);
        metadata.seekp(0, std::ios::end);
        metadata << "\"" << entry.path().string() << "\" ";
        state.loadedPlaylistFilepaths.push_back(entry.path().string());
        state.playlistAddFromFolderTab.addedFile = true;
      }
      lf_pop_style_props();
      lf_unset_image_color();
      },
      nullptr,
      state.playlistAddFromFolderTab.folderContents, supportedFileFormats, false);


  beginBottomNavBar();
  backButtonTo(state.previousTab, [&](){
      if(state.playlistAddFromFolderTab.addedFile) {
      if(state.soundHandler.isInit) {
      state.soundHandler.stop();
      state.soundHandler.uninit();
      state.currentSoundFile = nullptr;
      }
      Playlist& playlist = state.playlists[state.currentPlaylist];

      playlist.musicFiles.clear();
      playlist.musicFiles.shrink_to_fit();
      state.loadedPlaylistFilepaths.clear();
      state.loadedPlaylistFilepaths.shrink_to_fit();

      state.loadedPlaylistFilepaths = PlaylistMetadata::getFilepaths(std::filesystem::directory_entry(playlist.path));
      loadPlaylistAsync(playlist);
      state.playlistAddFromFolderTab.addedFile = false;
      }
      });
  renderTrackMenu();
  lf_div_end();
}
static void renderPlaylistSetThumbnail() {
  lf_push_font(&state.h1Font);
  lf_text("Choose playlist thumbnail");
  lf_pop_font();

  lf_next_line();

  // File dialogue
  {
    static const std::vector<std::string> supportedFileFormats = 
    {".png", ".jpg"};
    static std::string currentPath = "";
    static std::vector<std::filesystem::directory_entry> folderContents;


    if(currentPath.empty()) {
      currentPath = std::string(getenv(HOMEDIR));
      folderContents = loadFolderContents(currentPath);
    }
    renderFileDialogue(
        [&](std::filesystem::directory_entry entry){
        if(entry.is_directory()) {
        currentPath = entry.path().string();
        folderContents.clear();
        folderContents = loadFolderContents(currentPath);
        lf_set_current_div_scroll(0.0f);
        lf_set_current_div_scroll_velocity(0.0f);
        } else if(entry.is_regular_file()) {
          if(state.previousTab == GuiTab::CreatePlaylist || state.previousTab == GuiTab::CreatePlaylistFromFolder) 
            state.createPlaylistTab.thumbnailPath = entry.path().string();
          else if(state.previousTab == GuiTab::Dashboard) {
            Playlist& currentPlaylist = state.playlists[state.currentPlaylist]; 
            currentPlaylist.thumbnailPath = entry.path().string();
            currentPlaylist.thumbnail = lf_load_texture(entry.path().string().c_str(), false, LF_TEX_FILTER_LINEAR); 
            bool playlistEmpty = currentPlaylist.musicFiles.empty();
            if(playlistEmpty) {
              for(auto& filepath : PlaylistMetadata::getFilepaths(std::filesystem::directory_entry(currentPlaylist.path))) {
                currentPlaylist.musicFiles.emplace_back((SoundFile){.path = filepath});
              }
            }
            Playlist::save(state.currentPlaylist);
            if(playlistEmpty) {
              currentPlaylist.musicFiles.clear();
              currentPlaylist.musicFiles.shrink_to_fit();
            }
          }
          changeTabTo(state.previousTab);
        }
        },
        [&](){
        currentPath = std::filesystem::path(currentPath).parent_path().string();
        folderContents.clear();
        folderContents = loadFolderContents(currentPath);
        },
        nullptr, 
        nullptr,
        nullptr,
        folderContents, supportedFileFormats, false);
  }
  beginBottomNavBar();
  backButtonTo(state.previousTab);
  renderTrackMenu();
  lf_div_end();
}

void renderSearchPlaylist() {
  lf_push_font(&state.h2Font);
  lf_text("Browse Playlist");
  lf_pop_font();
  lf_next_line();
  {
    LfUIElementProps props = input_field_style();
    props.margin_top = 15.0f;
    props.margin_bottom = 30.0f;
    lf_push_style_props(props);
    lf_input_text(&state.searchPlaylistInput.input);
    lf_pop_style_props();
  }

  lf_next_line();
  bool clickedThumbnail = false;
  SoundFile clickedSoundFile;
  if (!state.searchPlaylistResults.empty()) {
    lf_div_begin(LF_PTR, ((vec2s){(float)state.win->getWidth() - DIV_START_X * 2 - state.sideNavigationWidth, 
          (float)state.win->getHeight() - DIV_START_Y * 2 - lf_get_ptr_y() - 
          (BACK_BUTTON_HEIGHT + BACK_BUTTON_MARGIN_BOTTOM)}), true);

    const float margin = 30; 
    const vec2s size = (vec2s){PLAYLIST_FILE_THUMBNAIL_SIZE.x * 3.0f, PLAYLIST_FILE_THUMBNAIL_SIZE.y * 3.0f + 50.0f};
    const LfColor color = lf_color_brightness(LYSSA_BACKGROUND_COLOR, 0.8f);
    const float ptrXStart = lf_get_ptr_x();
    const float cornerRadius = 6.0f;

    uint32_t resIdx = 0;
    for(auto& res : state.searchPlaylistResults) {
      LfClickableItemState thumbnailState = renderSoundFileThumbnail((vec2s){size.x, size.x}, res, nullptr, true, 4.0f); 
      if(thumbnailState == LF_CLICKED) {
        state.currentSoundFile = &res;
        if(state.onTrackTab.trackThumbnail.width != 0) {
          lf_free_texture(&state.onTrackTab.trackThumbnail);
        }
        state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path, (vec2s){-1, -1});
        changeTabTo(GuiTab::OnTrack);
        playlistPlayFileWithIndex(resIdx, state.currentPlaylist);
      }
      lf_set_cull_end_x(lf_get_ptr_x());
      renderTextRaw((vec2s){lf_get_ptr_x() - size.x, lf_get_ptr_y() + size.x + (margin / 3.0f)}, res.title.c_str(),
          state.h6Font, LF_WHITE);

      renderTextRaw((vec2s){lf_get_ptr_x() - size.x, lf_get_ptr_y() + size.x + state.h6Font.font_size + (margin / 3.0f)}, res.artist.c_str(),
          state.h6Font, lf_color_brightness(GRAY, 1.4f));
      lf_unset_cull_end_x();

      lf_set_ptr_x_absolute(lf_get_ptr_x() + margin); 
      if(lf_get_ptr_x() + size.x >= state.win->getWidth()) {
        lf_set_ptr_x_absolute(ptrXStart); 
        lf_set_ptr_y_absolute(lf_get_ptr_y() + size.y + margin); 
      }
      if(thumbnailState == LF_HOVERED && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_RIGHT)) {
        clickedThumbnail = true;
        clickedSoundFile = res;
      }
      resIdx++;
    }
    lf_div_end();
    } else {
      LfUIElementProps props = lf_get_theme().text_props;
      props.margin_top = 150.0f;
      lf_push_style_props(props);

      const char* text = "There are no matches :(";
      lf_set_ptr_x_absolute(((state.win->getWidth() + state.sideNavigationWidth) - lf_text_dimension(text).x) / 2.0f);
      lf_text(text);
      lf_pop_style_props();

      lf_next_line();

      const char* subtext = "Try searching for something else.";
      lf_set_ptr_x_absolute(((state.win->getWidth() + state.sideNavigationWidth) - lf_text_dimension(subtext).x) / 2.0f);
      props.text_color = lf_color_brightness(GRAY, 1.2f);
      props.margin_top = 5.0f;
      lf_push_style_props(props);
      lf_text(subtext);
      lf_pop_style_props();
  } 
  if(clickedThumbnail) {
    state.popups[PopupType::PlaylistFileDialoguePopup] = std::make_unique<PlaylistFileDialoguePopup>(clickedSoundFile.path, 
        (vec2s){(float)lf_get_mouse_x() + 10, (float)lf_get_mouse_y() + 10});
    state.popups[PopupType::PlaylistFileDialoguePopup]->shouldRender = true;
  }

  beginBottomNavBar();
  backButtonTo(GuiTab::OnPlaylist);
  renderTrackMenu();
}
void renderSearchAll() {
  lf_text("Nothin");
}

  void renderFileDialogue(
      std::function<void(std::filesystem::directory_entry)> clickedEntryCb, 
      std::function<void()> clickedBackCb, 
      std::function<void()> renderTopBarCb, 
      std::function<void(std::filesystem::directory_entry, bool)> renderIconCb,
      std::function<bool(std::filesystem::directory_entry, bool)> renderPerEntryCb,
      const std::vector<std::filesystem::directory_entry>& folderContents, 
      const std::vector<std::string>& selectedExtensions,
      bool renderDirectoriesOnly)
{
  static bool renderHiddenFiles = false;

  bool clickedBackBtn = false;
  LfTexture backIcon = (LfTexture){
    .id = state.icons["back"].id, 
      .width = BACK_BUTTON_WIDTH / 2,
      .height = BACK_BUTTON_HEIGHT / 2
  };

  const LfColor barColor = lf_color_brightness(GRAY, 0.5);
  LfUIElementProps props = lf_get_theme().button_props;
  props.border_width = 0.0f;
  props.corner_radius = 4.0f;
  props.color = barColor; 
  lf_push_style_props(props);
  if(lf_image_button_fixed(backIcon, 50, -1) == LF_CLICKED) {
    if(clickedBackCb)
      clickedBackCb();
    clickedBackBtn = true;
  }
  lf_pop_style_props();

  
  if(renderTopBarCb)
    renderTopBarCb();
  {
    LfUIElementProps props = secondary_button_style();
    lf_push_style_props(props);
    if(lf_button_fixed("Toggle hidden files", -1, BACK_BUTTON_HEIGHT / 2.0f) == LF_CLICKED) {
      renderHiddenFiles = !renderHiddenFiles; 
    }
    lf_pop_style_props();
  }
  lf_next_line();


  LfUIElementProps divProps = lf_get_theme().div_props;
  divProps.color = lf_color_brightness(GRAY, 0.4);
  divProps.corner_radius = 10.0f;
  divProps.padding = 10.0f;

  float divMarginButton = 15;

  lf_push_style_props(divProps);
  lf_div_begin(((vec2s){lf_get_ptr_x() + DIV_START_X - lf_get_theme().button_props.margin_left, lf_get_ptr_y() + DIV_START_Y}), (((vec2s){(float)lf_get_current_div().aabb.size.x - (DIV_START_X * 4),
          (float)lf_get_current_div().aabb.size.y - DIV_START_Y * 2 - lf_get_ptr_y() - (BACK_BUTTON_HEIGHT + BACK_BUTTON_MARGIN_BOTTOM) - divMarginButton})), true);
  lf_pop_style_props();

  vec2s initialPtr = LF_PTR;

  if(clickedBackBtn) {
    lf_set_current_div_scroll(0.0f);
    lf_set_current_div_scroll_velocity(0.0f);
  }

  const vec2s iconSize = (vec2s){25, 25};

  uint32_t renderedEntryCount = 0;
  for(auto& entry : folderContents) {
    if(!entry.is_directory() && renderDirectoriesOnly) continue;
    if(entry.path().filename().string().front() == '.' && !renderHiddenFiles) continue;
    if(!selectedExtensions.empty()) {
      if(std::find(selectedExtensions.begin(), selectedExtensions.end(), entry.path().extension().string()) == selectedExtensions.end() 
          && entry.is_regular_file())
        continue;
    }
    LfAABB aabb = (LfAABB){
      .pos = (vec2s){lf_get_current_div().aabb.pos.x, lf_get_ptr_y()},
        .size = (vec2s){lf_get_current_div().aabb.size.x, iconSize.y}
    };

    bool hoveredEntry = lf_hovered(aabb.pos, aabb.size);
    if(hoveredEntry) {
      lf_rect_render(aabb.pos, (vec2s){aabb.size.x, aabb.size.y + 5.0f}, (LfColor){100, 100, 100, 255}, LF_NO_COLOR, 0.0f, 3.0f);
    }

    bool onClientUI = false;
    if(renderPerEntryCb)
      onClientUI = renderPerEntryCb(entry, hoveredEntry);


    if(renderIconCb) {
      renderIconCb(entry, hoveredEntry);
    } else {
      LfTexture icon = (LfTexture){
        .id = (entry.is_directory()) ? state.icons["folder"].id : state.icons["file"].id, 
          .width = (uint32_t)iconSize.x, 
          .height = (uint32_t)iconSize.y
      };
      LfUIElementProps props = lf_get_theme().button_props;
      props.margin_top = 0.0f;
      props.color = LF_NO_COLOR;
      props.padding = 2.5f;
      props.border_width = 0.0f;
      lf_push_style_props(props);
      lf_image_button(icon); 
      lf_pop_style_props();
    }

    props = lf_get_theme().text_props;
    props.margin_top = (iconSize.y - lf_text_dimension(entry.path().filename().string().c_str()).y) / 2.0f;
    lf_push_style_props(props);
    lf_text(entry.path().filename().string().c_str());
    lf_pop_style_props();

    if(hoveredEntry && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT)) {
      if(clickedEntryCb && !onClientUI)
        clickedEntryCb(entry);
      break;
    }
    lf_next_line();
    renderedEntryCount++;
  }
  if(renderedEntryCount == 0) {
    lf_text(renderDirectoriesOnly ? "There are no directories here" : "This directory is empty.");
  }

  lf_div_end();
}

void renderTrackDisplay() {
  if(state.currentSoundFile == NULL) return;
  const float margin = DIV_START_X;
  const float marginThumbnail = 15;
  const vec2s thumbnailContainerSize = PLAYLIST_FILE_THUMBNAIL_SIZE;
  const float padding = 10;

  std::filesystem::path filepath = state.currentSoundFile->path;

  std::string filename = state.currentSoundFile->title == "" ? 
    removeFileExtensionW(state.currentSoundFile->path.filename().string()) : state.currentSoundFile->title;
  std::string artist = state.currentSoundFile->artist;

  SoundFile& playingFile = state.playlists[state.playingPlaylist].musicFiles[state.playlists[state.playingPlaylist].playingFile];

  // Container 
  float containerPosX = (float)(state.win->getWidth() - state.trackProgressSlider.width) / 2.0f + state.trackProgressSlider.width + 
    lf_text_dimension(formatDurationToMins(state.soundHandler.lengthInSeconds).c_str()).x + margin;
  vec2s containerSize = (vec2s){
    .x = (state.win->getWidth() - margin) - containerPosX,
      .y = thumbnailContainerSize.y + padding * 2.0f
  };

  vec2s containerPos = (vec2s){
    containerPosX,
      (float)state.win->getHeight() - containerSize.y - margin
  };

  lf_rect_render(containerPos, containerSize, lf_color_brightness(GRAY, 0.5), LF_NO_COLOR, 0.0f, 4.5f); 

  // Thumbnail
  {
    lf_set_ptr_x_absolute(containerPos.x + padding);
    lf_set_ptr_y_absolute(containerPos.y + padding);
    renderSoundFileThumbnail(thumbnailContainerSize, playingFile);
  }
  // Name + Artist
  {
    float textLabelHeight = lf_text_dimension(filename.c_str()).y + lf_text_dimension(artist.c_str()).y + 5.0f;
    lf_set_line_should_overflow(false);
    lf_set_cull_end_x(containerPos.x + containerSize.x - padding);
    renderTextRaw(
        (vec2s){containerPos.x + padding + thumbnailContainerSize.x + marginThumbnail, 
        containerPos.y + padding + (thumbnailContainerSize.y -  textLabelHeight) / 2.0f},
        filename.c_str(), state.h6Font, LF_WHITE); 

    renderTextRaw(
        (vec2s){containerPos.x + padding + thumbnailContainerSize.x + marginThumbnail, 
        containerPos.y + padding + (thumbnailContainerSize.y - textLabelHeight) / 2.0f + lf_text_dimension(artist.c_str()).y + 5.0f},
        artist.c_str(), state.h6Font, lf_color_brightness(GRAY, 1.4));
    lf_unset_cull_end_x();
    lf_set_line_should_overflow(true);
  }
}
void renderTrackProgress(bool dark) {
  if(state.currentSoundFile == NULL) return;
  // Progress position in seconds
  state.trackProgressSlider.width = state.win->getWidth() / 2.5f;
  state.trackProgressSlider.height = 5.0f;
  state.trackProgressSlider.min = 0.0f;
  state.trackProgressSlider.max = state.soundHandler.lengthInSeconds;
  state.trackProgressSlider.handle_size = 15.0f;
  {
    lf_push_font(&state.h6Font);
    std::string durationMins = formatDurationToMins(state.soundHandler.getPositionInSeconds());
    lf_set_ptr_x_absolute((state.win->getWidth() - state.trackProgressSlider.width) / 2.0f - lf_text_dimension(durationMins.c_str()).x - 15);
    LfUIElementProps props = lf_get_theme().text_props;
    props.margin_top = 55.0f;
    props.margin_left = lf_text_dimension(durationMins.c_str()).x + 15;
    lf_push_style_props(props);
    lf_text(durationMins.c_str());
    lf_pop_style_props();
    lf_pop_font();
  }
  // Total sound duration
  {
    lf_push_font(&state.h6Font);
    lf_set_ptr_x_absolute((state.win->getWidth() - state.trackProgressSlider.width) / 2.0f + state.trackProgressSlider.width + 5);
    LfUIElementProps props = lf_get_theme().text_props;
    std::string durationMins = formatDurationToMins(state.soundHandler.lengthInSeconds);
    props.margin_top = 55.0f;
    props.margin_left = -lf_text_dimension(durationMins.c_str()).x - 7.5f;
    lf_push_style_props(props);
    lf_text(durationMins.c_str());
    lf_pop_style_props();
    lf_pop_font();
  }
  // Progress Bar 
  {
    lf_set_ptr_x_absolute((state.win->getWidth() - state.trackProgressSlider.width) / 2.0f);

    LfUIElementProps props = lf_get_theme().slider_props;
    props.margin_top = 40;
    props.margin_left = 0;
    props.margin_right = 0;
    props.corner_radius = 1.5;
    props.color = dark ? (LfColor){255, 255, 255, 30} : GRAY;
    props.text_color = LF_WHITE;
    props.border_width = 0;
    lf_push_style_props(props);

    vec2s posPtr = (vec2s){lf_get_ptr_x() + props.margin_left, lf_get_ptr_y() + props.margin_top};

    LfClickableItemState progressBar = lf_slider_int(&state.trackProgressSlider);

    lf_rect_render(posPtr, (vec2s){(float)state.trackProgressSlider.handle_pos, (float)state.trackProgressSlider.height}, LF_WHITE, LF_NO_COLOR, 0.0f, props.corner_radius);

    if(progressBar == LF_RELEASED || progressBar == LF_CLICKED) {
      state.soundHandler.setPositionInSeconds(state.currentSoundPos);
    }

    lf_pop_style_props();
  }
  lf_next_line();

  // Controls
  {
    vec2s iconSize = (vec2s){40, 40};
    vec2s iconSizeSm = (vec2s){28, 28};
    float iconMargin = 20;
    float controlWidth = (iconSizeSm.x) * 4 + iconSize.x + (iconMargin * 4);
    LfUIElementProps props = lf_get_theme().button_props;
    props.color = LF_NO_COLOR;
    props.border_width = 0; 
    props.corner_radius = 0; 
    props.margin_top = -10.0f + (iconSize.x - iconSizeSm.x) / 2.0f - 5;
    props.margin_left = 0;
    props.margin_right = iconMargin;
    props.padding = 0;

    lf_set_ptr_x_absolute((state.win->getWidth() - controlWidth) / 2.0f);
    lf_push_style_props(props);

    lf_set_image_color(lf_color_brightness(GRAY, 2.5f));

    if(lf_image_button(((LfTexture){.id = state.icons[(state.shuffle ? "shuffle_active" : (dark ? "shuffle_dark" : "shuffle"))].id, 
              .width = (uint32_t)iconSizeSm.x, .height = (uint32_t)iconSizeSm.y})) == LF_CLICKED) {
      state.shuffle = !state.shuffle;
    }

    if(lf_image_button(((LfTexture){.id = state.icons[dark ? "skip_down_dark" : "skip_down"].id, .width = (uint32_t)iconSizeSm.x, .height = (uint32_t)iconSizeSm.y})) == LF_CLICKED) {
      skipSoundDown(state.playingPlaylist);
    }
    lf_pop_style_props();
    {
      LfUIElementProps playProps = props; 
      playProps.margin_top = -15.0f;
      playProps.padding = 0;
      lf_push_style_props(playProps);   
      if(lf_image_button(((LfTexture){.id = state.soundHandler.isPlaying ? state.icons[dark ? "pause_dark" : "pause"].id : state.icons[dark ? "play_dark" : "play"].id, 
              .width = (uint32_t)iconSize.x, .height = (uint32_t)iconSize.y})) == LF_CLICKED) {
        if(state.soundHandler.isPlaying)
          state.soundHandler.stop();
        else 
          state.soundHandler.play();
      } 
      lf_pop_style_props();
    }
    lf_push_style_props(props);
    if(lf_image_button(((LfTexture){.id = state.icons[dark ? "skip_up_dark" : "skip_up"].id, .width = (uint32_t)iconSizeSm.x, .height = (uint32_t)iconSizeSm.y})) == LF_CLICKED) {
      skipSoundUp(state.playingPlaylist);
    }
    lf_pop_style_props();
    props.margin_right = 0;
    lf_push_style_props(props);
    if(lf_image_button(((LfTexture){.id = state.icons[(state.replayTrack ? "replay_active" : (dark ? "replay_dark" : "replay"))].id, 
            .width = (uint32_t)iconSizeSm.x, .height = (uint32_t)iconSizeSm.y})) == LF_CLICKED) {
      state.replayTrack = !state.replayTrack;
    }
    lf_unset_image_color();
    lf_pop_style_props();
  }
}
void renderTrackMenu() {
  renderTrackVolumeControl();
  if(state.currentTab != GuiTab::OnTrack) {
    renderTrackProgress();
    renderTrackDisplay();
  }
}
void beginBottomNavBar() {
  lf_div_begin(((vec2s){DIV_START_X, state.win->getHeight() - BACK_BUTTON_HEIGHT - 45 - DIV_START_Y * 2}), ((vec2s){(float)state.win->getWidth(), BACK_BUTTON_HEIGHT + 45 + DIV_START_Y * 2}),
      false);
}

void updateFullscreenTrackTab() {
  state.trackFullscreenTab.uiTimer += state.deltaTime;
  if(state.trackFullscreenTab.uiTimer >= state.trackFullscreenTab.uiTime) {
    state.trackFullscreenTab.showUI = false;
  }
  if(lf_mouse_move_event().happened || lf_key_event().happened) {
    state.trackFullscreenTab.showUI = true;
    state.trackFullscreenTab.uiTimer = 0.0f;
  }
}

void renderTrackVolumeControl() {
  // Sound Control
  {
    LfUIElementProps props = lf_get_theme().button_props;
    LfTexture icon;
    if(state.soundHandler.volume == 0.0f) {
      icon = state.icons["volume_mute"];
    } else if(state.soundHandler.volume > VOLUME_MAX / 2.0f) {
      icon = state.icons["volume_high"];
    } else {
      icon = state.icons["volume_low"];
    }

    const vec2s iconSize = (vec2s){BACK_BUTTON_HEIGHT, BACK_BUTTON_HEIGHT};

    props.color = LF_NO_COLOR;
    props.border_color = LF_NO_COLOR;
    props.border_width = 0.0f;
    props.margin_top = 10.0f;
    lf_push_style_props(props);

    bool overControlArea = lf_hovered((vec2s){lf_get_ptr_x() + props.margin_left, lf_get_ptr_y() + props.margin_top}, (vec2s){
        (float)state.win->getWidth(), iconSize.y + props.margin_top + props.margin_bottom});

    LfClickableItemState soundButton = lf_image_button(((LfTexture){.id = icon.id, .width = (uint32_t)iconSize.x, .height = (uint32_t)iconSize.y}));

    if(!state.showVolumeSliderTrackDisplay) {
      overControlArea = (soundButton == LF_HOVERED);
    }
    if(overControlArea && !state.showVolumeSliderTrackDisplay) {
      state.showVolumeSliderTrackDisplay = true; 
    }
    else if(!overControlArea && !state.volumeSlider.held && !state.showVolumeSliderOverride) {
      state.showVolumeSliderTrackDisplay = false;
    }
    else if(soundButton == LF_CLICKED) { 
      if(state.soundHandler.volume != 0.0f) {
        state.volumeBeforeMute = state.soundHandler.volume;
      }
      state.soundHandler.volume = (state.soundHandler.volume != 0.0f) ? 0.0f : state.volumeBeforeMute;
    }
    lf_pop_style_props();
  }
  if(state.showVolumeSliderTrackDisplay) {
    LfUIElementProps props = lf_get_theme().slider_props;
    props.corner_radius = 1.5;
    props.color = (LfColor){255, 255, 255, 30};
    props.text_color = LF_WHITE;
    props.border_width = 0;
    props.margin_top = 40;
    lf_push_style_props(props);

    lf_rect_render((vec2s){lf_get_ptr_x() + props.margin_left, lf_get_ptr_y() + props.margin_top}, (vec2s){(float)state.volumeSlider.handle_pos, (float)state.volumeSlider.height}, 
        props.text_color, LF_NO_COLOR, 0.0f, props.corner_radius);

    LfClickableItemState sliderState = lf_slider_int(&state.volumeSlider);
    lf_pop_style_props();

    if(VOLUME_SHOW_PERCENT) {
      std::stringstream volumeSS;
      volumeSS << state.soundHandler.volume;
      volumeSS << "%";

      props.margin_top -= lf_text_dimension(volumeSS.str().c_str()).y;
      lf_push_style_props(props);

      if(sliderState == LF_CLICKED || sliderState == LF_HELD) {
        lf_text((volumeSS.str().c_str()));
      }
      lf_pop_style_props();
    }
  } 
}
void backButtonTo(GuiTab tab, const std::function<void()>& clickCb ) {
  lf_next_line();

  LfUIElementProps props = lf_get_theme().button_props;
  props.color = (LfColor){0, 0, 0, 0};
  props.border_width = 0;
  lf_push_style_props(props);

  if(lf_image_button(((LfTexture){.id = state.icons["back"].id, .width = BACK_BUTTON_WIDTH, .height = BACK_BUTTON_HEIGHT})) == LF_CLICKED) {
    if(clickCb)
      clickCb();
    changeTabTo(tab);
  }

  lf_pop_style_props();
}

void loadPlaylists() {
  uint32_t playlistI = 0;

  if(std::find(state.playlists.begin(), state.playlists.end(), (Playlist){.path = LYSSA_DIR + "/playlists/favourites"}) == state.playlists.end()) {
    std::string favouritesDir = LYSSA_DIR + "/playlists/favourites";
    Playlist favourites;
    favourites.path = favouritesDir;
    favourites.name = PlaylistMetadata::getName(std::filesystem::directory_entry(favouritesDir));
    favourites.desc = PlaylistMetadata::getDesc(std::filesystem::directory_entry(favouritesDir));
    favourites.url = "";
    favourites.thumbnailPath = "";
    state.playlists.emplace_back(favourites);
  }

  for (const auto& folder : std::filesystem::directory_iterator(LYSSA_DIR + "/playlists/")) {
    if(folder.path().filename() == "favourites") continue;
    Playlist playlist{};
    playlist.path = folder.path().string();
    playlist.name = PlaylistMetadata::getName(folder);
    playlist.desc = PlaylistMetadata::getDesc(folder);
    playlist.url = PlaylistMetadata::getUrl(folder);
    playlist.thumbnailPath = PlaylistMetadata::getThumbnailPath(folder);
    if(std::find(state.playlists.begin(), state.playlists.end(), playlist) == state.playlists.end()) {
      if(playlist.thumbnailPath != "") {
        playlist.thumbnail = lf_load_texture_resized(playlist.thumbnailPath.string().c_str(), false, LF_TEX_FILTER_LINEAR, 180, 180);
      }
      state.playlists.emplace_back(playlist);
    }
  }
}

void loadPlaylistFileAsync(std::vector<SoundFile>* files, std::string path) {
  std::lock_guard<std::mutex> lock(state.mutex);
  SoundFile file{};
  if(std::filesystem::exists(path)) {
    file.path = std::filesystem::path(path); 
    file.thumbnail = (LfTexture){0};
    file.duration = SoundTagParser::getSoundDuration(path);
    file.artist = SoundTagParser::getSoundArtist(path);
    file.title = SoundTagParser::getSoundTitle(path);
    file.releaseYear = SoundTagParser::getSoundReleaseYear(path);
  } else {
    file.path = "File cannot be loaded";
    file.thumbnail = (LfTexture){0};
    file.duration = 0; 
  }
  files->emplace_back(file);
  if(std::filesystem::exists(path)) {
    state.playlistFileThumbnailData.emplace_back(SoundTagParser::getSoundThubmnailData(path, PLAYLIST_FILE_THUMBNAIL_SIZE));
  } else {
    state.playlistFileThumbnailData.emplace_back((TextureData){0});
  }
}

void addFileToPlaylistAsync(std::vector<SoundFile>* files, std::string path, uint32_t playlistIndex) {
  std::lock_guard<std::mutex> lock(state.mutex);
  Playlist& playlist = state.playlists[playlistIndex];

  std::ofstream metadata(playlist.path.string() + "/.metadata", std::ios::app);

  if(!metadata.is_open()) return;

  std::ifstream playlistFile(path);
  if(!playlistFile.good()) return;

  metadata << std::string("\"" + path + "\" ");
  metadata.close();

  SoundFile file{};
  if(std::filesystem::exists(path)) {
    file.path = std::filesystem::path(path); 
    file.thumbnail = (LfTexture){0};
    file.duration = SoundTagParser::getSoundDuration(path);
    file.artist = SoundTagParser::getSoundArtist(path);
    file.title = SoundTagParser::getSoundTitle(path);
    file.releaseYear = SoundTagParser::getSoundReleaseYear(path);
  } else {
    file.path = "File cannot be loaded";
    file.thumbnail = (LfTexture){0};
    file.duration = 0; 
  }
  files->emplace_back(file);
  if(std::filesystem::exists(path)) {
    state.playlistFileThumbnailData.emplace_back(SoundTagParser::getSoundThubmnailData(path, PLAYLIST_FILE_THUMBNAIL_SIZE));
  } else {
    state.playlistFileThumbnailData.emplace_back((TextureData){0});
  }
}

std::vector<std::string> loadFilesFromFolder(const std::filesystem::path& folderPath) {
  std::vector<std::string> files;
  for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
    if (std::filesystem::is_directory(entry.path()) && entry.path().filename().string()[0] != '.') {
      files = loadFilesFromFolder(entry.path());
    } else if (std::filesystem::is_regular_file(entry.path())) {
      files.emplace_back(entry.path().string());
    }
  } 
  return files;
}

void moveFileInPlaylistIdx(uint32_t playlistIndex, uint32_t fromIndex, uint32_t toIndex) {
  std::vector<SoundFile>& files = state.playlists[playlistIndex].musicFiles;
  if (fromIndex < 0 || fromIndex >= files.size() || toIndex < 0 || toIndex >= files.size()) {
    LOG_ERROR("Index out of range. files.size(): %i, fromIndex: %i, toIndex: %i", (int32_t)files.size(), fromIndex, toIndex);
  }

  SoundFile element = files[fromIndex];
  if (fromIndex < toIndex) {
    files.insert(files.begin() + toIndex + 1, element);
    files.erase(files.begin() + fromIndex);
  } else {
    files.erase(files.begin() + fromIndex);
    files.insert(files.begin() + toIndex, element);
  }
}

void playlistPlayFileWithIndex(uint32_t i, uint32_t playlistIndex) {
  if(!state.playlistFileFutures.empty()) return;
  Playlist& playlist = state.playlists[playlistIndex];
  playlist.playingFile = i;
  playlist.selectedFile = i;

  if(state.soundHandler.isPlaying)
    state.soundHandler.stop();

  if(state.soundHandler.isInit)
    state.soundHandler.uninit();

  state.soundHandler.init(playlist.musicFiles[i].path.string(), miniaudioDataCallback);
  state.soundHandler.play();

  state.currentSoundPos = 0.0;
  state.trackProgressSlider.max = state.soundHandler.lengthInSeconds;

  if(state.playingPlaylist != playlistIndex) {
    state.alreadyPlayedTracks.clear();
  }
  state.playingPlaylist = playlistIndex;

  if(std::find(state.alreadyPlayedTracks.begin(), state.alreadyPlayedTracks.end(), i) == state.alreadyPlayedTracks.end()) {
    state.alreadyPlayedTracks.push_back(i);
  }
  if(state.alreadyPlayedTracks.size() >= state.playlists[state.currentPlaylist].musicFiles.size()) {
    state.alreadyPlayedTracks.clear();
  }
}

void skipSoundUp(uint32_t playlistInedx) {
  Playlist& playlist = state.playlists[playlistInedx];

  if(!state.shuffle) {
    if(playlist.playingFile + 1 < playlist.musicFiles.size())
      playlist.playingFile++;
    else 
      playlist.playingFile = 0;
  } else {
    RandomEngine random(0, playlist.musicFiles.size() - 1);
    playlist.playingFile = random.randInt();
    while(std::find(state.alreadyPlayedTracks.begin(), state.alreadyPlayedTracks.end(), playlist.playingFile) != state.alreadyPlayedTracks.end()) {
      playlist.playingFile = random.randInt();
    }
  }

  state.currentSoundFile = &playlist.musicFiles[playlist.playingFile];
  if(state.currentTab == GuiTab::OnTrack || state.currentTab == GuiTab::TrackFullscreen) {
    if(state.onTrackTab.trackThumbnail.width != 0) {
      lf_free_texture(&state.onTrackTab.trackThumbnail);
    }
    state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path);
  }

  playlistPlayFileWithIndex(playlist.playingFile, playlistInedx);
  float filePosY = playlist.musicFiles[playlist.playingFile].renderPosY;
  playlist.scroll = -filePosY;
}

void skipSoundDown(uint32_t playlistIndex) {
  Playlist& playlist = state.playlists[playlistIndex];

  if(playlist.playingFile - 1 >= 0)
    playlist.playingFile--;
  else 
    playlist.playingFile = playlist.musicFiles.size() - 1; 

  state.currentSoundFile = &playlist.musicFiles[playlist.playingFile];
  if(state.currentTab == GuiTab::OnTrack || state.currentTab == GuiTab::TrackFullscreen) {
    if(state.onTrackTab.trackThumbnail.width != 0)
      lf_free_texture(&state.onTrackTab.trackThumbnail);
    state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path);
  }

  playlistPlayFileWithIndex(playlist.playingFile, playlistIndex);
  float filePosY = playlist.musicFiles[playlist.playingFile].renderPosY;
  playlist.scroll = -filePosY;
}
void updateSoundProgress() {
  if(!state.soundHandler.isInit) {
    return;
  }

  if(state.currentSoundPos + 1 <= state.soundHandler.lengthInSeconds && state.soundHandler.isPlaying) {
    state.soundPosUpdateTime += state.deltaTime;
    if(state.soundPosUpdateTime >= state.soundPosUpdateTimer) {
      state.soundPosUpdateTime = 0.0f;
      state.currentSoundPos++;
    }
  }

  if(state.currentSoundPos >= (uint32_t)state.soundHandler.lengthInSeconds && !state.trackProgressSlider.held) {
    if(!state.replayTrack) {
      skipSoundUp(state.currentPlaylist);
    } else {
      state.currentSoundPos = 0.0f;
      state.soundHandler.setPositionInSeconds(state.currentSoundPos);
    }
  }
}
std::string removeFileExtension(const std::string& filename) {
  // Find the last dot (.) in the filename
  size_t lastDotIndex = filename.rfind('.');

  if (lastDotIndex != std::string::npos && lastDotIndex > 0) {
    return filename.substr(0, lastDotIndex);
  } else {
    return filename;
  }
}

void terminateAudio() {
  if(!state.soundHandler.isInit || !ASYNC_PLAYLIST_LOADING) return;
  state.soundHandler.stop();
  state.soundHandler.uninit();
  state.previousSoundFile = state.currentSoundFile;
  state.previousSoundPos = state.currentSoundPos;
  state.currentSoundFile = nullptr;
  state.playlists[state.currentPlaylist].playingFile = -1;
}

std::string removeFileExtensionW(const std::string& filename) {
  // Find the last dot (.) in the filename
  size_t lastDotIndex = filename.rfind('.');

  if (lastDotIndex != std::string::npos && lastDotIndex > 0) {
    return filename.substr(0, lastDotIndex);
  } else {
    return filename;
  }
}

void loadIcons() {
  for (const auto& entry : std::filesystem::directory_iterator(LYSSA_DIR + "/assets/textures/")) {
    if (entry.path().extension() == ".png") {
      state.icons[entry.path().stem()] = lf_load_texture(entry.path().c_str(), true, LF_TEX_FILTER_LINEAR); 
    }
  }
}

bool playlistFileOrderCorrect(uint32_t playlistIndex, const std::vector<std::string>& paths) {
  const std::vector<SoundFile>& soundFiles = state.playlists[playlistIndex].musicFiles;

  if (soundFiles.size() != paths.size()) {
    return false;
  }

  for (size_t i = 0; i < soundFiles.size(); ++i) {
    if (soundFiles[i].path != paths[i]) {
      return false;
    }
  }

  return true;
}

std::vector<SoundFile> reorderPlaylistFiles(const std::vector<SoundFile>& soundFiles, const std::vector<std::string>& paths) {
  std::vector<SoundFile> reorderedVector;
  reorderedVector.reserve(soundFiles.size());

  for (const auto& path : paths) {
    for (const auto& file : soundFiles) {
      if (file.path == path) {
        reorderedVector.emplace_back(file);
        break;
      }
    }
  }

  return reorderedVector;
}

static bool compareSoundFilesByName(const SoundFile& a, const SoundFile& b) {
    std::string filenameA = removeFileExtensionW(a.path.filename().string());
    std::string filenameB = removeFileExtensionW(b.path.filename().string());
    return filenameA < filenameB;
}
static bool compareTextureDataByName(const TextureData& a, const TextureData& b) {
    return removeFileExtension(a.path.filename()) < removeFileExtension(b.path.filename());
}

void handleAsyncPlaylistLoading() {
  for (uint32_t i = 0; i < state.playlistFileThumbnailData.size(); i++) {
    SoundFile& file = state.playlists[state.currentPlaylist].musicFiles[i];
    if(file.loaded) continue;
    file.loaded = true;
  }
  if(state.loadedPlaylistFilepaths.size() == state.playlists[state.currentPlaylist].musicFiles.size() && !state.playlistFileFutures.empty()) {
    // Create OpenGL Textures for the thumbnails that were loaded
    for (uint32_t i = 0; i < state.playlistFileThumbnailData.size(); i++) {
      SoundFile& file = state.playlists[state.currentPlaylist].musicFiles[i];
      LfTexture& thumbnail = file.thumbnail;
      TextureData data = state.playlistFileThumbnailData[i];
      lf_create_texture_from_image_data(LF_TEX_FILTER_LINEAR, &thumbnail.id, data.width, data.height, data.channels, data.data);
      thumbnail.width = data.width;
      thumbnail.height = data.height;
    } 
    if(state.currentPlaylist != -1) {
      for (auto &future : state.playlistFileFutures) {
        future.get(); // Wait for each future to complete
      } 
      state.playlistFileFutures.clear(); 
      Playlist& playlist = state.playlists[state.currentPlaylist];
      std::sort(playlist.musicFiles.begin(), playlist.musicFiles.end(), compareSoundFilesByName);
      std::sort(state.playlistFileThumbnailData.begin(), state.playlistFileThumbnailData.end(), compareTextureDataByName);

      if(state.previousSoundFile) {
        Playlist& playingPlaylist = state.playlists[state.playingPlaylist];
        auto it = std::find(playingPlaylist.musicFiles.begin(), playingPlaylist.musicFiles.end(), (SoundFile){.path = state.previousSoundFile->path});
        if(it != playingPlaylist.musicFiles.end()) {
          uint32_t index = std::distance(playingPlaylist.musicFiles.begin(), it);
          playlistPlayFileWithIndex(index, state.playingPlaylist);
          state.currentSoundFile = state.previousSoundFile;
          state.currentSoundPos = state.previousSoundPos;
          state.soundHandler.setPositionInSeconds(state.currentSoundPos);
        }
      }
    }
  }
}

void loadPlaylistAsync(Playlist& playlist) {
  playlist.musicFiles.clear();
  state.playlistFileThumbnailData.clear();
  state.playlistFileThumbnailData.shrink_to_fit();


  for(auto& path : state.loadedPlaylistFilepaths) {
    if(!Playlist::containsFile(path, state.currentPlaylist)){ 
      if(ASYNC_PLAYLIST_LOADING) {
        state.playlistFileFutures.emplace_back(std::async(std::launch::async, loadPlaylistFileAsync, &state.playlists[state.currentPlaylist].musicFiles, path));
     } else {
        SoundFile file;
        if(std::filesystem::exists(std::filesystem::path(path))) {
          SoundMetadata metadata = SoundTagParser::getSoundMetadataNoThumbnail(path); 
          file = (SoundFile){
            .path = path,
              .artist = metadata.artist, 
              .title = metadata.title,
              .releaseYear = metadata.releaseYear,
              .duration = static_cast<int32_t>(metadata.duration),
              .thumbnail = SoundTagParser::getSoundThubmnail(path, PLAYLIST_FILE_THUMBNAIL_SIZE),
          };

        } else {
          file = (SoundFile){
            .path = "File cannot be loaded",
              .duration = 0, 
              .thumbnail = (LfTexture){0}
          };
        }
        playlist.musicFiles.emplace_back(file);
      }
    }
  }
  if(!ASYNC_PLAYLIST_LOADING) {
    std::sort(playlist.musicFiles.begin(), playlist.musicFiles.end(), compareSoundFilesByName);
  }
}

LfClickableItemState renderSoundFileThumbnail(vec2s thumbnailContainerSize, SoundFile& file, const std::function<void()>& clickCb, bool uiResponse, float cornerRadius) {
  LfTexture thumbnail = (file.thumbnail.width == 0) ? state.icons["music_note"] : file.thumbnail;
  float aspect = (float)thumbnail.width / (float)thumbnail.height;
  float thumbnailHeight = thumbnailContainerSize.y / aspect; 
  LfUIElementProps props = lf_get_theme().button_props;
  props.color = PLAYLIST_FILE_THUMBNAIL_COLOR;
  props.border_width = 0.0f;
  props.corner_radius = cornerRadius != -1 ? cornerRadius : PLAYLIST_FILE_THUMBNAIL_CORNER_RADIUS;
  props.padding = 0.0f;
  props.margin_left = 0.0f;
  props.margin_top = 0.0f;
  props.margin_right = 0.0f;
  props.margin_bottom = 0.0f;
  lf_push_style_props(props);
  LfClickableItemState thumbnailState = lf_item(thumbnailContainerSize);
  if(thumbnailState == LF_CLICKED && state.playlistFileFutures.empty() && uiResponse) {
    state.currentSoundFile = &file;
    if(state.onTrackTab.trackThumbnail.width != 0) {
      lf_free_texture(&state.onTrackTab.trackThumbnail);
    }
    state.onTrackTab.trackThumbnail = SoundTagParser::getSoundThubmnail(state.currentSoundFile->path, (vec2s){-1, -1});
    changeTabTo(GuiTab::OnTrack);

    if(clickCb)
      clickCb();
  }
  lf_pop_style_props();

  if(thumbnailHeight >= thumbnailContainerSize.y - 10) {
    thumbnailHeight = thumbnailContainerSize.y;
  }
  lf_image_render((vec2s){lf_get_ptr_x() - thumbnailContainerSize.x, lf_get_ptr_y() + 
      (thumbnailContainerSize.y - thumbnailHeight) / 2.0f}, LF_WHITE,
      (LfTexture){.id = thumbnail.id, .width = (uint32_t)thumbnailContainerSize.x, .height = (uint32_t)thumbnailHeight}, LF_NO_COLOR, 0.0f, 
      thumbnailHeight >= thumbnailContainerSize.y ? PLAYLIST_FILE_THUMBNAIL_CORNER_RADIUS : 0.0f); 
  return thumbnailState;
}

bool renderMenuBarElement(const std::string& text, uint32_t iconId) {
  const uint32_t iconSize = 24;
  const float divMargin = 10.0f;
  float divWidth = iconSize + 
    lf_get_theme().button_props.margin_right + 
    lf_get_theme().text_props.margin_left +
    lf_text_dimension(text.c_str()).x + 
    lf_get_theme().text_props.margin_right;
  float divHeight = iconSize + lf_get_theme().button_props.margin_top * 3.0f;
  bool onDiv = lf_area_hovered(LF_PTR, (vec2s){divWidth, divHeight});
  {
    LfUIElementProps props = lf_get_theme().div_props;
    props.color = onDiv ? (LfColor){255, 255, 255, 20} : LF_NO_COLOR;
    props.corner_radius = 4.0f;
    lf_push_style_props(props);
    lf_div_begin(LF_PTR, ((vec2s){divWidth, divHeight}), false);
    lf_pop_style_props();
  }
  {
    LfUIElementProps icon_props = lf_get_theme().button_props;
    icon_props.color = LF_NO_COLOR;
    icon_props.padding = 0.0f;
    icon_props.border_width = 0.0f;

    lf_push_style_props(icon_props);
    lf_set_image_color(lf_color_brightness(GRAY, 2.0f));
    lf_image_button(((LfTexture){.id = iconId, .width = iconSize, .height = iconSize}));
    lf_unset_image_color();
    lf_pop_style_props();

    LfUIElementProps text_props = lf_get_theme().text_props;
    text_props.margin_top = icon_props.margin_top + ((iconSize - lf_text_dimension(text.c_str()).y) / 2.0f);
    lf_push_style_props(text_props);
    lf_push_font(&state.h6Font);
    lf_text(text.c_str());
    lf_pop_style_props();
    lf_pop_font();
  }
  lf_div_end();
  lf_set_ptr_x_absolute(lf_get_ptr_x() + divWidth + divMargin);

  return onDiv && lf_mouse_button_is_released(GLFW_MOUSE_BUTTON_LEFT);
}

std::vector<SoundFile> matchSoundFiles(const std::vector<SoundFile>& files, const std::string& searchTerm) {
  std::vector<SoundFile> matches;
  std::string searchTermLower = LyssaUtils::toLower(std::string(searchTerm.begin(), searchTerm.end())); 
  for (const auto& soundFile : files) {
    std::string titleLower = LyssaUtils::toLower(soundFile.title);
    if (titleLower.find(searchTermLower) != std::string::npos) {
      matches.push_back(soundFile);
    }
  }
  return matches;
}
void searchPlaylistInputInsertCb(void* inputData) {
  LfInputField* input = (LfInputField*)inputData;
  lf_input_insert_char_idx(input, lf_char_event().charcode, input->cursor_index++);
  state.searchPlaylistResults = matchSoundFiles(state.playlists[state.currentPlaylist].musicFiles, state.searchPlaylistInput.buffer);
}

void searchPlaylistInputKeyCb(void* inputData) {
  state.searchPlaylistResults = matchSoundFiles(state.playlists[state.currentPlaylist].musicFiles, state.searchPlaylistInput.buffer);
}

LfTextProps renderTextRaw(vec2s pos, const std::string& text, LfFont font, LfColor color, float wrapPoint, vec2s stopPoint, bool noRender) {
  return lf_text_render(pos, text.c_str(), font, color, 
      wrapPoint, stopPoint, 
      noRender, false, -1, -1);
}

std::string formatDurationToMins(int32_t duration) {
  int32_t minutes = duration / 60;
  int32_t seconds = duration % 60;
  std::stringstream format;
  format  << std::setw(2) << std::setfill('0') << minutes << ":" 
    << std::setw(2) << std::setfill('0') << seconds;
  return format.str();
}

int main(int argc, char* argv[]) {
  // Initialization 
  initWin(WIN_START_W, WIN_START_H); 
  initUI();

  if(!std::filesystem::exists(LYSSA_DIR)) { 
    std::filesystem::create_directory(LYSSA_DIR);
  }
  loadPlaylists();

  // Creating the popups

  vec4s clearColor = lf_color_to_zto(LYSSA_BACKGROUND_COLOR); 

  while(!state.win->shouldClose()) { 
    state.sideNavigationWidth = MAX(state.win->getWidth() / 6.4f, 175.0f);

    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    if(ASYNC_PLAYLIST_LOADING)
      handleAsyncPlaylistLoading();

    // Updating the timestamp of the currently playing sound
    updateSoundProgress();
    updateFullscreenTrackTab();

    if(state.playlistThumbnailDownloadIndex != -1) {
      if(LyssaUtils::getCommandOutput("pgrep yt-dlp") == "") {
        Playlist& playlist = state.playlists[state.playlistThumbnailDownloadIndex];
        if(playlist.thumbnail.width != 0) {
          lf_free_texture(&playlist.thumbnail);
        }
        playlist.thumbnail = lf_load_texture_resized(playlist.thumbnailPath.string().c_str(), false, LF_TEX_FILTER_LINEAR, 180, 180);
        state.playlistThumbnailDownloadIndex = -1;
      }
    }

    // Delta-Time calculation
    float currentTime = glfwGetTime();
    state.deltaTime = currentTime - state.lastTime;
    state.lastTime = currentTime;

    if(!WIN_VSYNC) {
      float frameTime = 1.0f / TARGET_FRAME_RATE;
      if(state.deltaTime < frameTime) {
        float sleepTime = frameTime - state.deltaTime;
        glfwWaitEventsTimeout(sleepTime);
      }
    }

    // OpenGL color clearing 
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);

    lf_begin();

    renderDashboardNav();

    vec2s divStart = (vec2s){state.currentTab == GuiTab::OnTrack ? 0.0f : state.sideNavigationWidth + DIV_START_X, DIV_START_Y};

    lf_div_begin(divStart, ((vec2s){(float)state.win->getWidth() - divStart.x, (float)state.win->getHeight() - divStart.y}), true);

    switch(state.currentTab) {
      case GuiTab::Dashboard:
        renderDashboard();
        break;
      case GuiTab::CreatePlaylist:
        renderCreatePlaylist();
        break;
      case GuiTab::CreatePlaylistFromFolder:
        renderCreatePlaylistFromFolder();
        break;
      case GuiTab::DownloadPlaylist: 
        renderDownloadPlaylist();
        break;
      case GuiTab::OnPlaylist:
        renderOnPlaylist();
        break;
      case GuiTab::OnTrack:
        renderOnTrack();
        break;
      case GuiTab::TrackFullscreen:
        renderTrackFullscreen();
        break;
      case GuiTab::PlaylistAddFromFile:
        renderPlaylistAddFromFile();
        break;
      case GuiTab::PlaylistAddFromFolder:
        renderPlaylistAddFromFolder();
        break;
      case GuiTab::PlaylistSetThumbnail:
        renderPlaylistSetThumbnail();
        break;
      case GuiTab::SearchPlaylist:
        renderSearchPlaylist();
        break;
      case GuiTab::SearchAll:
        renderSearchAll();
        break;
      default:
        lf_text("Page not found");
        break;
    }
    if(!lf_input_grabbed())
      handleTabKeyStrokes();

    lf_div_end();

    for(const auto& popup : state.popups) {
      if(popup.second->shouldRender) {
        popup.second->render();
      }
      popup.second->update();
    }
    state.infoCards.render();
    state.infoCards.update();

    lf_end();

    glfwPollEvents();
    state.win->swapBuffers();
  }
  if(state.playlistDownloadRunning) {
    system("pkill yt-dlp");
  }
  return 0;
} 
