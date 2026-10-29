/* Dumps every Interface\*.xml from each MPQ given on the command line into
   outdir/<n>/<path>, plus an index line "n<TAB>archive<TAB>path<TAB>size". */
#include <StormLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
static void mkdirs(char* p) { for (char* s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; } }
int main(int argc, char** argv) {
  const char* out = argv[1];
  for (int a = 2; a < argc; a++) {
    HANDLE m; if (!SFileOpenArchive(argv[a], 0, MPQ_OPEN_READ_ONLY, &m)) { fprintf(stderr, "open fail %s\n", argv[a]); continue; }
    SFILE_FIND_DATA fd; HANDLE f = SFileFindFirstFile(m, "Interface\\*", &fd, NULL);
    while (f) {
      size_t n = strlen(fd.cFileName);
      if (n > 4 && !strcasecmp(fd.cFileName + n - 4, ".xml")) {
        HANDLE h; if (SFileOpenFileEx(m, fd.cFileName, 0, &h)) {
          DWORD sz = SFileGetFileSize(h, NULL), rd = 0; char* buf = malloc(sz + 1);
          if (SFileReadFile(h, buf, sz, &rd, NULL) || rd == sz) {
            char path[4096]; snprintf(path, sizeof path, "%s/%d/%s", out, a, fd.cFileName);
            for (char* s = path; *s; s++) if (*s == '\\') *s = '/';
            mkdirs(path); FILE* o = fopen(path, "wb"); fwrite(buf, 1, rd, o); fclose(o);
            printf("%d\t%s\t%s\t%u\n", a, argv[a], fd.cFileName, rd);
          }
          free(buf); SFileCloseFile(h);
        }
      }
      if (!SFileFindNextFile(f, &fd)) { SFileFindClose(f); break; }
    }
    SFileCloseArchive(m);
  }
  return 0;
}
