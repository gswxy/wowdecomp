/* Parses each XML file named on stdin the way XMLTree_Load does (one buffer,
   isFinal=1, XML_ParserCreate(NULL), element + character-data handlers) and
   prints the event stream; text is coalesced between element events. */
#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char* text; static size_t tlen, tcap;
static void put(const char* s, int n) {
  for (int i = 0; i < n; i++) { unsigned char c = s[i];
    if (c == '\\') fputs("\\\\", stdout); else if (c == '\n') fputs("\\n", stdout);
    else if (c < 32 || c > 126) printf("\\x%02x", c); else putchar(c); }
}
static void flush(void) { if (tlen) { fputs("T ", stdout); put(text, tlen); putchar('\n'); tlen = 0; } }
static void chars(void* u, const XML_Char* s, int n) {
  if (tlen + n > tcap) { tcap = (tlen + n) * 2; text = realloc(text, tcap); }
  memcpy(text + tlen, s, n); tlen += n;
}
static void start(void* u, const XML_Char* name, const XML_Char** atts) {
  flush(); fputs("S ", stdout); put(name, strlen(name));
  for (; *atts; atts += 2) { fputs(" @", stdout); put(atts[0], strlen(atts[0])); putchar('='); put(atts[1], strlen(atts[1])); }
  putchar('\n');
}
static void end(void* u, const XML_Char* name) { flush(); fputs("E ", stdout); put(name, strlen(name)); putchar('\n'); }
int main(void) {
  char path[4096]; XML_Parser p = NULL;
  while (fgets(path, sizeof path, stdin)) {
    path[strcspn(path, "\n")] = 0;
    FILE* f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = malloc(n + 1); fread(buf, 1, n, f); fclose(f);
    if (p) XML_ParserReset(p, NULL); else p = XML_ParserCreate(NULL);
    XML_SetElementHandler(p, start, end); XML_SetCharacterDataHandler(p, chars);
    printf("FILE %s\n", path); tlen = 0;
    int ok = XML_Parse(p, buf, (int)n, 1); flush();
    if (ok) puts("RESULT ok");
    else printf("RESULT error %d line %lu col %lu: %s\n", (int)XML_GetErrorCode(p),
                (unsigned long)XML_GetCurrentLineNumber(p), (unsigned long)XML_GetCurrentColumnNumber(p), XML_ErrorString(XML_GetErrorCode(p)));
    free(buf);
  }
  return 0;
}
