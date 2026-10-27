#include "game_maze.h"
#include "ui.h"
#include "joystick.h"
#include "storage.h"
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

// ============================================================
//  MAZE — Professional glitch-free implementation
//
//  Rendering contract:
//    • erasePlayer: redraws EXACTLY the previous cell pixels
//      (background or goal marker) — no full-screen redraws
//    • drawPlayer:  blits a filled circle into current cell
//    • HUD: updated only when elapsed second changes
//    • Pulse animation: erase → redraw in same cell, no
//      side effects on adjacent walls/passages
//    • Movement: erase old cell, draw new cell, check win
// ============================================================

static inline int SW()     { return (int)uiTft().width(); }
static inline int SH()     { return (int)uiTft().height(); }
static inline int bodyY0() { return uiBodyY0(); }
static inline int bodyH()  { return uiBodyH(); }

// ---- Maze ----
static const int MAZE_W=21, MAZE_H=15;   // must be odd
static uint8_t maze[MAZE_W][MAZE_H];
static const int GX=MAZE_W-2, GY=MAZE_H-2;   // goal pos (fixed)

// ---- Cell geometry — computed once on enter ----
static int CW, CH;          // cell pixel size
static int OX, OY;          // maze origin in pixels

static inline int cellPX(int mx) { return OX+mx*CW; }
static inline int cellPY(int my) { return OY+my*CH; }
static inline int cellCX(int mx) { return cellPX(mx)+CW/2; }
static inline int cellCY(int my) { return cellPY(my)+CH/2; }

// ---- Player ----
static int  PX, PY;         // player maze coords
static int  LPX, LPY;       // last maze coords

// ---- State ----
static bool     gameWon, timeoutShown;
static uint32_t startTime, winTime;
static int      bestSecs, lastElapsed;
static const int TIME_LIMIT=120;

// ---- Pulse ----
static uint8_t  pulseR;
static bool     pulseGrow;
static uint32_t lastPulse;

// ---- Move throttle ----
static uint32_t lastMove;

// ============================================================
//  Wall colour (depth gradient)
// ============================================================
static uint16_t wallColor(int x,int y){
  int d=x+y, maxD=MAZE_W+MAZE_H;
  uint8_t r=(uint8_t)(d*8/maxD);
  uint8_t b=(uint8_t)(31-d*16/maxD);
  return (uint16_t)((r<<11)|b);
}

// ============================================================
//  Maze generation — iterative DFS to avoid stack overflow
// ============================================================
static void generateMaze() {
  // Clear
  for(int x=0;x<MAZE_W;x++) for(int y=0;y<MAZE_H;y++) maze[x][y]=0;

  // Simple iterative backtracker using a small stack
  // Stack holds cell coords (packed as x*100+y)
  static int stk[MAZE_W*MAZE_H/2];
  int top=0;
  maze[1][1]=1;
  stk[top++]=1*100+1;

  int dx[4]={0,2,0,-2}, dy[4]={-2,0,2,0};

  while(top>0){
    int cell=stk[top-1];
    int cx=cell/100, cy=cell%100;
    // Shuffle directions
    int dirs[4]={0,1,2,3};
    for(int i=0;i<4;i++){int j=(int)(esp_random()%4);int t=dirs[i];dirs[i]=dirs[j];dirs[j]=t;}
    bool found=false;
    for(int i=0;i<4;i++){
      int nx=cx+dx[dirs[i]], ny=cy+dy[dirs[i]];
      if(nx>0&&nx<MAZE_W-1&&ny>0&&ny<MAZE_H-1&&maze[nx][ny]==0){
        maze[cx+dx[dirs[i]]/2][cy+dy[dirs[i]]/2]=1;
        maze[nx][ny]=1;
        stk[top++]=nx*100+ny;
        found=true;
        break;
      }
    }
    if(!found) top--;
  }
  maze[GX][GY]=1;  // ensure goal is open
}

// ============================================================
//  Drawing
// ============================================================
static void drawMazeFull() {
  auto& T=uiTft();
  for(int x=0;x<MAZE_W;x++){
    for(int y=0;y<MAZE_H;y++){
      int px=cellPX(x), py=cellPY(y);
      T.fillRect(px, py, CW, CH, maze[x][y]==0 ? wallColor(x,y) : uiColorBg());
    }
  }
  // Goal marker
  int gx=cellCX(GX), gy=cellCY(GY);
  uiTft().fillCircle(gx, gy, CW/2-1, 0x07E0);
  uiTft().drawCircle(gx, gy, CW/2,   0xFFE0);
  // Start marker
  uiTft().drawRect(cellPX(1), cellPY(1), CW, CH, uiColorAccent());
}

// Redraw one maze cell to its base state (wall or passage or goal)
static void redrawCell(int mx, int my) {
  int px=cellPX(mx), py=cellPY(my);
  if(maze[mx][my]==0){
    uiTft().fillRect(px,py,CW,CH,wallColor(mx,my));
  } else if(mx==GX&&my==GY){
    uiTft().fillRect(px,py,CW,CH,uiColorBg());
    uiTft().fillCircle(cellCX(GX),cellCY(GY),CW/2-1,0x07E0);
    uiTft().drawCircle(cellCX(GX),cellCY(GY),CW/2,  0xFFE0);
  } else {
    uiTft().fillRect(px,py,CW,CH,uiColorBg());
  }
}

static void drawPlayer(){
  int cx=cellCX(PX), cy=cellCY(PY);
  uiTft().fillCircle(cx, cy, pulseR+1, uiColorAccent());
  uiTft().fillCircle(cx, cy, (pulseR>1)?pulseR-1:1, uiColorText());
}

static void drawHUD(uint32_t now){
  auto& T=uiTft();
  // Snake-style: 16-px cleared strip with PAD side margins.
  const int PAD=6;
  T.fillRect(PAD, bodyY0()+2, SW()-2*PAD, 16, uiColorBg());
  T.setFont(nullptr); T.setTextSize(1);
  int elapsed=(int)((now-startTime)/1000);
  int remain=TIME_LIMIT-elapsed; if(remain<0)remain=0;
  T.setTextColor(remain<20?0xF800:uiColorSub(), uiColorBg());
  T.setCursor(PAD, bodyY0()+5);
  char buf[16]; snprintf(buf,sizeof(buf),"Time:%d",remain); T.print(buf);
  T.setTextColor(0xFFE0,uiColorBg());
  T.setCursor(SW()/2, bodyY0()+5);
  if(bestSecs<9999) snprintf(buf,sizeof(buf),"Best:%ds",bestSecs);
  else              snprintf(buf,sizeof(buf),"Best:--");
  T.print(buf);
}

static void drawWinOverlay(int secs){
  auto& T=uiTft();
  int bw=SW()-30,bh=80,bx2=15,by2=bodyY0()+(bodyH()-bh)/2;
  T.fillRoundRect(bx2,by2,bw,bh,8,uiColorBg());
  T.drawRoundRect(bx2,by2,bw,bh,8,0x07E0);
  T.drawRoundRect(bx2+2,by2+2,bw-4,bh-4,6,0xFFE0);
  T.setFont(&FreeSansBold12pt7b); T.setTextColor(0x07E0);
  int16_t x1,y1; uint16_t w,h;
  T.getTextBounds("YOU WIN!",0,0,&x1,&y1,&w,&h);
  T.setCursor(bx2+(bw-w)/2,by2+32); T.print("YOU WIN!");
  T.setFont(&FreeSans9pt7b); T.setTextColor(uiColorText());
  char buf[40];
  snprintf(buf,sizeof(buf),"%ds  Best:%s%ds",secs,(secs==bestSecs?"NEW! ":""),bestSecs);
  T.getTextBounds(buf,0,0,&x1,&y1,&w,&h);
  T.setCursor(bx2+(bw-w)/2,by2+62); T.print(buf);
  T.setFont(nullptr);
}

static void drawTimeoutOverlay(){
  auto& T=uiTft();
  int bw=SW()-30,bh=60,bx2=15,by2=bodyY0()+(bodyH()-bh)/2;
  T.fillRoundRect(bx2,by2,bw,bh,8,uiColorBg());
  T.drawRoundRect(bx2,by2,bw,bh,8,0xF800);
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(0xF800,uiColorBg());
  T.setCursor(bx2+10,by2+16); T.print("TIME'S UP!");
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(bx2+10,by2+34); T.print("Tap/Enc to try again");
}

static void footer(const char* s){
  auto& T=uiTft();
  int fy=SH()-uiFooterH();
  T.fillRect(0,fy,SW(),uiFooterH(),uiColorBg());
  T.drawFastHLine(0,fy,SW(),uiColorDivider());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(8,fy+uiFooterH()/2+4);
  T.print(s?s:"");
}

// ============================================================
//  Movement helper — erase old, move, draw new, check win
// ============================================================
static void tryMove(int nx,int ny){
  if(nx<0||nx>=MAZE_W||ny<0||ny>=MAZE_H) return;
  if(!maze[nx][ny]) return;
  redrawCell(PX,PY);     // restore old cell
  LPX=PX; LPY=PY;
  PX=nx;  PY=ny;
  drawPlayer();
  lastMove=millis();
}

// ============================================================
//  Enter
// ============================================================
void mazeEnter(){
  storageBegin();
  bestSecs=storageGetI32("maze_best",9999);

  generateMaze();

  // Compute fixed cell size
  CW = SW()/MAZE_W;
  CH = (bodyH()-20)/MAZE_H;
  OX = (SW()-MAZE_W*CW)/2;
  OY = bodyY0()+20;

  PX=1; PY=1; LPX=1; LPY=1;
  gameWon=false; timeoutShown=false;
  startTime=millis(); winTime=0;
  pulseR=4; pulseGrow=true;
  lastPulse=millis(); lastMove=0; lastElapsed=-1;

  uiShowScreenTitle("MAZE");
  uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
  drawMazeFull();
  drawHUD(millis());
  drawPlayer();
  footer("Joy/Swipe:Move  Long:Menu");
}

// ============================================================
//  Loop
// ============================================================
void mazeLoop(uint32_t now){
  if(gameWon){
    if(winTime==0) winTime=now;
    if(now-winTime>=4000) mazeEnter();   // auto restart after 4s
    return;
  }
  int elapsed=(int)((now-startTime)/1000);
  if(elapsed>=TIME_LIMIT){
    if(!timeoutShown){ drawTimeoutOverlay(); timeoutShown=true; }
    return;
  }
  // HUD every second
  if(elapsed!=lastElapsed){ lastElapsed=elapsed; drawHUD(now); }

  // Pulse animation (120ms period)
  if(now-lastPulse>=120){
    lastPulse=now;
    redrawCell(PX,PY);   // restore cell background first
    if(pulseGrow){ pulseR++; if(pulseR>=5) pulseGrow=false; }
    else         { pulseR--; if(pulseR<=2) pulseGrow=true;  }
    drawPlayer();
  }

  // Joystick movement (130ms repeat)
  if(now-lastMove>130){
    JoyDir dir=joyReadDir();
    int nx=PX,ny=PY;
    if(dir==JOY_UP)    ny--;
    if(dir==JOY_DOWN)  ny++;
    if(dir==JOY_LEFT)  nx--;
    if(dir==JOY_RIGHT) nx++;
    if(nx!=PX||ny!=PY) tryMove(nx,ny);
    else lastMove=now;  // still advance timer (no spam)
  }

  // Win check
  if(PX==GX&&PY==GY&&!gameWon){
    gameWon=true;
    bool nb=(elapsed<bestSecs);
    if(nb){bestSecs=elapsed;storageSetI32("maze_best",bestSecs);}
    drawWinOverlay(elapsed);
  }
}

// ============================================================
//  Touch
// ============================================================
void mazeOnTouch(const TouchEvent& ev){
  int elapsed=(int)((millis()-startTime)/1000);
  if(elapsed>=TIME_LIMIT||gameWon){ mazeEnter(); return; }
  if(ev.type==TE_SWIPE_UP)    tryMove(PX,PY-1);
  if(ev.type==TE_SWIPE_DOWN)  tryMove(PX,PY+1);
  if(ev.type==TE_SWIPE_LEFT)  tryMove(PX-1,PY);
  if(ev.type==TE_SWIPE_RIGHT) tryMove(PX+1,PY);
}
void mazeOnEncoderDelta(int d){ (void)d; }
void mazeOnEncoderClick(){ mazeEnter(); }
