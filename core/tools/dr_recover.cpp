// dr_recover: repairs a project folder after a crash.
//
//   dr_recover <project-dir>
//
// Closes cursor.json / events.json left open mid-write and reports what was
// kept. Fragmented MP4 tracks need no repair to play; the native apps finalize
// their duration on next launch (P1).
#include <cstdio>
#include <filesystem>
#include <string>

#include "dr/dr_core.h"
#include "dr/file_util.h"
#include "dr/input_log.h"
#include "dr/project.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <project-dir>\n", argv[0]);
    return 2;
  }
  std::string dir = argv[1];
  int status = 0;
  dr::LoadError lerr;
  auto project = dr::load_project(dir, &lerr);
  if (!project) {
    std::fprintf(stderr, "project.json: %s\n", lerr.message.c_str());
    status = 1;
  } else {
    std::printf("project %s (%s)\n", project->id.c_str(),
                project->state == dr::ProjectState::Complete ? "complete" : "interrupted");
  }
  for (const char* name : {dr::kCursorFileName, dr::kEventsFileName}) {
    std::string path = dr::join_path(dir, name);
    if (!std::filesystem::exists(path)) continue;
    std::string err;
    auto r = dr::recover_row_log(path, &err);
    if (!r) {
      std::fprintf(stderr, "%s: %s\n", name, err.c_str());
      status = 1;
      continue;
    }
    std::printf("%s: %s, %llu rows\n", name, r->changed ? "repaired" : "ok",
                static_cast<unsigned long long>(r->rows_kept));
  }
  return status;
}
