/**
 * Cardputerを使ったラーメンタイマー
 */
#include "M5Cardputer.h"
/**
 * デフォルトのタイムアウト値
 */
#define DEFAULT_TIMEOUT (3 * 60 * 1000)

/**
 * start, end 状態のとき何も処理されず30秒経過すると電源オフ
 */
#define NOOP_TIMEOUT (30 * 1000)

//キャレット位置 999:59 のそれぞれの桁に対応する
#define CARRET_M00  4
#define CARRET_0M0  3
#define CARRET_00M  2
#define CARRET_S0  1
#define CARRET_0S  0
#define CARRET_MAX  4
#define CARRET_MIN  0
// init_timeout はカウントダウン開始時のミリ秒数。start状態で
// 表示するのは init_timeout
int init_timeout = DEFAULT_TIMEOUT;
// こちらのタイムアウトはカウントダウン用
// カウント状態のときはこの値を表示する
// 設定する値と、カウントに実際に使う値は別の設計となっている。
int timeout = 0;
// キー入力有効
bool keyboardEnabled = true;
/**
 * 描画タスクに渡すメッセージの型
 */
typedef struct {
  // 種別
  char type;
  // 時間表示文字列
  char timestring[8];
  // キャレット位置
  int carret;
} DispMsg_t;
// DispMsg_tのtypeを意味づける
// 時間が更新された
#define DISP_MSG_TIMEUPDATED_MASK 1
// キャレットが動いた
#define DISP_MSG_CARRETMOVED_MASK 2

// 描画タスクキュー
QueueHandle_t qDispMsg = NULL;
// キーインプットキュー
QueueHandle_t qKeyInput = NULL;

// ラーメンタイマの状態
typedef enum enum_ramentimer_state{
  STATE_START,
  STATE_COUNTING,
  STATE_FANFARE,
  STATE_END
} ramentimer_state_t;
// updateの各状態の dispatch table
typedef ramentimer_state_t (*state_update_func_t)(void);
/**
 * @brief 更新関数（START状態用）
 * @param  なし
 * @return なし
 */
ramentimer_state_t update_start(void);
/**
 * @brief 更新関数（カウントダウン状態用）
 * @param  なし
 * @return なし
 */
ramentimer_state_t update_counting(void);
/**
 * @brief 更新関数（ファンファーレ状態用）
 * @param  なし
 * @return なし
 */
ramentimer_state_t update_fanfare(void);
/**
 * @brief 更新関数（終了状態用）
 * @param  なし
 * @return なし
 */
ramentimer_state_t update_end(void);
// 前回のミリ秒保持（なるべく正確なdeltaTimeを求める）
uint32_t previous_millis=0;
// ラーメンタイマの状態
ramentimer_state_t state;
// 表示用文字列のバッファ
char disp_timestring[8] = "000:00.";
// キャレットのデフォルト値は分のところに合わせる
signed char carret = CARRET_00M;
/**
 * @brief 時間表示用のバッファ作成
 * @param min 分
 * @param sec 秒
 * @param dot_enabled 右端のドットをつけるかどうか
 * @param timestringbuff 出力バッファ
 */
void make_timestring(int min, int sec, int dot_enabled, char timestringbuff[]){
    timestringbuff[0] = '0' + (min / 100);
    timestringbuff[1] = '0' + ((min / 10) % 10);
    timestringbuff[2] = '0' + (min % 10);

    timestringbuff[3] = ':';

    timestringbuff[4] = '0' + (sec / 10);
    timestringbuff[5] = '0' + (sec % 10);

    timestringbuff[6] = dot_enabled ? '.' : ' ';

    timestringbuff[7] = '\0';
}
/**
 * @brief 共通の更新関数
 * @param  なし
 */
void update_common(void){
}
/**
 * @brief タイムアウト値の減算
 * @param  なし
 */
void sub_timeout(void){
  uint32_t now = millis();
  uint32_t delta =  millis() - previous_millis;
  previous_millis = now;
  timeout -= delta;
}
/**
 * @brief キャレット位置のミリ秒取得
 * @param carret 取得するキャレット位置
 * @return キャレット位置が１増えたときのミリ秒
 */
int carret_incrementvalue(int carret){
  if (carret==CARRET_0S){
    return 1000;
  }else if(carret==CARRET_S0){
    return 10*1000;
  }else if(carret==CARRET_00M){
    return 60*1000;
  }else if(carret==CARRET_0M0){
    return 10*60*1000;
  }else if(carret==CARRET_M00){
    return 100*60*1000;
  }else{
    return 0;
  }
}
/**
 * @brief キャレット桁のクリア
 * @param timeout 操作対象
 * @param carret キャレット位置
 * @return timeout のキャレット桁を 0 にしたときの値
 */
int carret_reset(int timeout, int carret){
  int m100 = timeout /(100*60*1000);
  int remain = timeout % (100*60*1000);
  int m10 = remain / (10*60*1000);
  remain = remain % (10*60*1000);
  int m1 = remain / (60*1000);
  remain = remain % (60*1000);
  int s10 = remain / (10*1000);
  remain = remain % (10*1000);
  int s1 = remain / 1000;
  return (carret==CARRET_M00?0:m100) * (100*60*1000) +
    (carret==CARRET_0M0?0:m10) * (10*60*1000) +
    (carret==CARRET_00M?0:m1) * (60*1000) +
    (carret==CARRET_S0?0:s10) * (10*1000) +
    (carret==CARRET_0S?0:s1) * (1000);
}
/**
 * @brief キャレット桁に値をセット
 * @param timeout 操作対象
 * @param carret キャレット位置
 * @param value セットする値
 * @return timeoutのキャレット位置にvalueをセットしたときのミリ秒
 */
int carret_set(int timeout, int carret, int value){
  return carret_reset(timeout, carret) + value * carret_incrementvalue(carret);
}
// 更新関数（START状態用）実装
ramentimer_state_t update_start(void){
  // 表示用メッセージ
  static DispMsg_t dispMsg;
  // キー入力格納
  static char input;
  // 時間、キャレットに変更があったか
  bool modified = false;
  update_common();
  sub_timeout();
  if (xQueueReceive(qKeyInput, &input, 0)==pdTRUE){
    // 何でも入力があれば、無操作カウントダウンをリセット
    timeout = NOOP_TIMEOUT;
    if (input == 0x0a && init_timeout >0){
      // enterが押されて
      // init_timeoutが0でないならたら状態遷移カウントダウン状態へ以降
      return STATE_COUNTING;
    }else if(input >='0' && input <='9'){
      if (carret == CARRET_S0 && input >= '6'){
        // 10秒の単位のところに指定できるのは'5'まで
        // それ以外のときは入力無視
      }else{
        // キャレット位置に値を設定
        init_timeout = carret_set(init_timeout, carret, input - '0');
        modified = true;
      }
    }else if (input ==';'){
      // up キャレット桁のミリ秒分加算
      init_timeout += carret_incrementvalue(carret);
      // 最大を超えるときは最大値
      init_timeout = (init_timeout > 999*60*1000+59*1000)?999*60*1000+59*1000:init_timeout;
      modified = true;
    }else if (input == '.'){
      // down キャレット桁のミリ秒分減算
      init_timeout -= carret_incrementvalue(carret);
      // 最小を下回るときは最小
      init_timeout = (init_timeout < 0)?0:init_timeout;
      modified = true;
    }else if (input == ','){
      // left キャレットを左に移動 
      carret ++;
      // 最大値を超えるときは最大値
      carret = (carret > CARRET_MAX)? CARRET_MAX: carret;
      modified = true;
    }else if (input =='/'){
      // right キャレットを右に移動
      carret --;
      // 最小値未満のときは最小値
      carret = (carret < CARRET_MIN)? CARRET_MIN: carret;
      modified = true;
    }else{
      // ignore any other characters
    }
  }
  // 時間に変更があったときは表示タスクにメッセージを送る
  if (modified){
      int min = init_timeout / 60000;
      int msec = init_timeout % 60000;
      int sec = msec / 1000;
      make_timestring(min, sec, false, dispMsg.timestring); 
      dispMsg.carret = carret;
      dispMsg.type = DISP_MSG_TIMEUPDATED_MASK|DISP_MSG_CARRETMOVED_MASK;
      xQueueSend(qDispMsg, &dispMsg, portMAX_DELAY);
  }
  // 無操作期間が経過した場合電源OFF
  if (timeout < 0){
    M5Cardputer.Power.powerOff();
  }
  // enterが押された場合以外は基本的に
  // 状態遷移しない。
  return STATE_START;
}
// 更新関数（カウントダウン状態用）実装
ramentimer_state_t update_counting(void){
  // 表示用メッセージ
  static DispMsg_t dispMsg;
  update_common();
  sub_timeout();
  // 表示用の時間に999ミリ秒を足す理由は人間の感覚としては
  // 残り時間10秒といったとき10.999〜10よりは
  // 10〜9.001秒までを10秒と扱ったほうが良いため
  // 前者は1秒も経過してないのに開始直後に秒が減るが
  // 後者は1秒経過後にはじめて1秒減る。
  int dispTimeout = timeout + 999;
  int min = dispTimeout / 60000;
  int msec = dispTimeout % 60000;
  int sec = msec / 1000;
  msec = msec % 1000;
  
  // ドットは動いてる感出すだけの演出
  int dot_enabled = false;
  
  if (msec<200){
    dot_enabled = true;
  }
  // 表示用文字列を作って表示タスクにメッセージを送る
  make_timestring(min, sec, dot_enabled, dispMsg.timestring); 
  dispMsg.type = DISP_MSG_TIMEUPDATED_MASK;
  xQueueSend(qDispMsg, &dispMsg, portMAX_DELAY);
  
  // タイムアウトが経過した場合ファンファーレ状態に遷移する
  if (timeout <= 0){
    return STATE_FANFARE;
  }
  // タイムアウトしない場合はカウント状態継続
  return STATE_COUNTING;
}
// 音源 今回の再生するメロディはチャルメラとした。
// そのため必要な音はG4、A5、A6 48000サンプリングレート
// で近い周波数の矩形波を生成するためのテーブル
// 392Hzで122個のテーブルが必要
int16_t wave392[122];
int16_t *wave440;
int16_t *wave494;
void init_wave(void)
{
    // 392Hzのテーブルの前半は最大値、後半は最小値
    // これで矩形波を表す
    for (int i = 0; i < 61; i++) {
        wave392[i] = INT16_MAX;
        wave392[i + 61] = INT16_MIN;
    }
    // 同じテーブルで開始位置を変えれば、データ量を増やさず
    // 違う周波数のバッファとすることができる
    wave440 = wave392 + 7;
    wave494 = wave392 + 12;
}
/**
 * @brief 楽譜エントリ用の構造体
 */
struct score_entry {
    // wave 配列 休符のとき0
    int16_t *wave;
    // wave 配列の長さ 休符のとき0
    int length;
    // 再生時間
    int msec;
    // 次の楽譜エントリ
    struct score_entry *next;
};
// 楽譜
struct score_entry score[44];

/**
 * @brief 楽譜の初期化
 * @param なし
 */
void init_score(void)
{
  //チャルメラ
  score[0] = (struct score_entry){wave392, 122, 125, &score[1]};
  score[1] = (struct score_entry){NULL, 0, 15, &score[2]};
  score[2] = (struct score_entry){wave440, 109, 125, &score[3]};
  score[3] = (struct score_entry){NULL, 0, 15, &score[4]};
  score[4] = (struct score_entry){wave494,  97, 545, &score[5]};
  score[5] = (struct score_entry){NULL, 0, 15, &score[6]};
  score[6] = (struct score_entry){wave440, 109, 125, &score[7]};
  score[7] = (struct score_entry){NULL, 0, 15, &score[8]};
  score[8] = (struct score_entry){wave392, 122, 125, &score[9]};
  score[9] = (struct score_entry){NULL, 0, 1120, &score[10]};
  score[10] = (struct score_entry){wave392, 122, 125, &score[11]};
  score[11] = (struct score_entry){NULL, 0, 15, &score[12]};
  score[12] = (struct score_entry){wave440, 109, 125, &score[13]};
  score[13] = (struct score_entry){NULL, 0, 15, &score[14]};
  score[14] = (struct score_entry){wave494,  97, 125, &score[15]};
  score[15] = (struct score_entry){NULL, 0, 15, &score[16]};
  score[16] = (struct score_entry){wave440, 109, 125, &score[17]};
  score[17] = (struct score_entry){NULL, 0, 15, &score[18]};
  score[18] = (struct score_entry){wave392, 122, 125, &score[19]};
  score[19] = (struct score_entry){NULL, 0, 15, &score[20]};
  score[20] = (struct score_entry){wave440, 109, 545, &score[21]};
  score[21] = (struct score_entry){NULL, 0, 980, &score[22]};
  score[22] = (struct score_entry){wave392, 122, 125, &score[23]};
  score[23] = (struct score_entry){NULL, 0, 15, &score[24]};
  score[24] = (struct score_entry){wave440, 109, 125, &score[25]};
  score[25] = (struct score_entry){NULL, 0, 15, &score[26]};
  score[26] = (struct score_entry){wave494,  97, 545, &score[27]};
  score[27] = (struct score_entry){NULL, 0, 15, &score[28]};
  score[28] = (struct score_entry){wave440, 109, 125, &score[29]};
  score[29] = (struct score_entry){NULL, 0, 15, &score[30]};
  score[30] = (struct score_entry){wave392, 122, 125, &score[31]};
  score[31] = (struct score_entry){NULL, 0, 1120, &score[32]};
  score[32] = (struct score_entry){wave392, 122, 125, &score[33]};
  score[33] = (struct score_entry){NULL, 0, 15, &score[34]};
  score[34] = (struct score_entry){wave440, 109, 125, &score[35]};
  score[35] = (struct score_entry){NULL, 0, 15, &score[36]};
  score[36] = (struct score_entry){wave494,  97, 125, &score[37]};
  score[37] = (struct score_entry){NULL, 0, 15, &score[38]};
  score[38] = (struct score_entry){wave440, 109, 125, &score[39]};
  score[39] = (struct score_entry){NULL, 0, 15, &score[40]};
  score[40] = (struct score_entry){wave392, 122, 125, &score[41]};
  score[41] = (struct score_entry){NULL, 0, 15, &score[42]};
  score[42] = (struct score_entry){wave440, 109, 545, &score[43]};
  score[43] = (struct score_entry){NULL, 0, 980, NULL};
}
//fanfare中で使うscoreのpointer
struct score_entry *wkScore = NULL;
// 更新関数（ファンファーレ状態用）実装
ramentimer_state_t update_fanfare(void){
  update_common();
  // sleeptimeは無音時に使う。
  static int32_t sleeptime = 0;
  // すでに再生中である場合はこの周期更新は終了
  if (M5Cardputer.Speaker.isPlaying()){
    return STATE_FANFARE;
  }
  // 演奏が止まっているなら sleeptime から delta 時間分減算
  uint32_t now = millis();
  int32_t delta = (int32_t)(millis() - previous_millis);
  previous_millis = now;
  if (sleeptime > 0){
    sleeptime -= delta;
  }
  // sleeptimeが残っているなら、この周期更新は終了、
  if (sleeptime > 0){
    return STATE_FANFARE;
  }
  // wkScore が NULL = これ以上再生する楽譜ない場合
  // 終了状態へ遷移
  if (wkScore == NULL){
    return STATE_END;
  }
  //再生WAVEが指定されていないときは休符の扱い
  if (wkScore->wave == NULL){
    sleeptime = wkScore->msec;
    // 次のnoteに移動
    wkScore = wkScore->next;
    return STATE_FANFARE;
  }
  // playRawのリピート回数の計算
  int repeat = wkScore->msec * 48000
           / (wkScore->length * 1000);
  M5Cardputer.Speaker.playRaw(wkScore->wave, wkScore->length, 48000,false,repeat); 
  // 次のnoteに移動
  wkScore = wkScore->next;
  return STATE_FANFARE;
}
// 更新関数（終了状態用）実装
ramentimer_state_t update_end(void){
  char input;
  update_common();
  sub_timeout();
  if (xQueueReceive(qKeyInput, &input, 0)==pdTRUE){
    // 何でも入力があれば、無操作カウントダウンをリセット
    timeout = NOOP_TIMEOUT;
    // 入力後 Start状態に遷移
    return STATE_START;
  }
  if (timeout < 0){
    M5Cardputer.Power.powerOff();
  }
  return STATE_END;
}
/**
 * @brief 入力をクリア
 * @param  なし
 */
void clearInput(void){
  char input;
  while (xQueueReceive(qKeyInput, &input, 0)==pdTRUE){
  }
}
/**
 * @brief リセット
 */
void reset(){
  DispMsg_t dispMsg;
  init_timeout = DEFAULT_TIMEOUT;
  timeout = NOOP_TIMEOUT;
  previous_millis = millis();
  keyboardEnabled = true;
  state = STATE_START;
  carret = CARRET_00M;
  int min = init_timeout / 60000;
  int msec = init_timeout % 60000;
  int sec = msec / 1000;
  make_timestring(min, sec, false, dispMsg.timestring); 
  dispMsg.type = DISP_MSG_TIMEUPDATED_MASK | DISP_MSG_CARRETMOVED_MASK;
  dispMsg.carret = carret;
  xQueueSend(qDispMsg, &dispMsg, portMAX_DELAY);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(10, 10);
  M5Cardputer.Display.println("Press ok to start.");
}
/**
 * @brief ファンファーレの準備
 * @param なし
 */
void prepare_fanfare(void){
  //楽譜を先頭に
  wkScore = score;
  //ディスプレイ表示後食い気味に来すぎるためディレイ追加
  vTaskDelay(pdMS_TO_TICKS(150));
}
/**
 * @brief 状態遷移
 * @param state 遷移元の状態
 * @param next_state 遷移先の状態
 * @return 想定された状態遷移の場合 0 想定外の場合 -1
 */
int transition(ramentimer_state_t state, ramentimer_state_t next_state){
  switch (state){
    case STATE_START: switch(next_state){
      case STATE_COUNTING:
        keyboardEnabled = false;
        clearInput();
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setCursor(10, 10);
        M5Cardputer.Display.println("                  ");
        // 設定した時間ｰをカウントダウン時間に
        timeout = init_timeout;
        return 0;
      case STATE_END:
        // START から ENDに行くのは、無操作タイムアウトの場合
        timeout = 0;
        clearInput();
        return 0;
      default:
        return -1;
    }
    case STATE_COUNTING: switch(next_state){
      case STATE_FANFARE:
        prepare_fanfare();
        return 0;
      default:
        return -1;
    }
    case STATE_FANFARE: switch(next_state){
      case STATE_END:
        clearInput();
        keyboardEnabled = true;
        // ファンファーレｰ>終了 無操作タイムアウト設定
        timeout = NOOP_TIMEOUT;
        return 0;
      default:
        return -1;
    }
    case STATE_END: switch (next_state)
    {
      case STATE_START:
        clearInput();
        keyboardEnabled = true;
        reset();
        return 0;
      default:
        return -1;
    }
    default:
      return -1;
  }
}
/**
 * @brief ディスパッチテーブル
 */
static const state_update_func_t dispatch_table[] = {
    [STATE_START]    = update_start,
    [STATE_COUNTING] = update_counting,
    [STATE_FANFARE]  = update_fanfare,
    [STATE_END]      = update_end
};
/**
 * @brief 制御タスク
 * @param parameters Unused. Don’t care.
 */
void controlTask(void *parameters) {
  reset();
  while (true){
    // ディスパッチテーブルに現在の状態を入れて各状態の更新を行う
    // リターンは次の状態
    ramentimer_state_t next_state = dispatch_table[state]();
    // 状態が変化するときは遷移関数に処理を委ねる
    if (next_state!= state){
      transition(state,next_state);
    }
    // 状態更新
    state = next_state;
    // Taskのディレイあまり長くするとチャルメラのリズム感が狂う
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
//キャレット位置に応じた表示文字列のテーブル
static const char* CURSOR_STRINGS[] = {
    "     -", // CARRET_0S (0)
    "    - ", // CARRET_S0 (1)
    "  -   ", // CARRET_00M (2)
    " -    ", // CARRET_0M0 (3)
    "-     "  // CARRET_M00 (4)
};
/**
 * @brief 画面表示タスク
 * @param parameters Unused. Don’t care.
 */
void displayTask(void *parameters) {
  DispMsg_t msg;
  char input;
  static int n = 0;
  while (true) {
      // 描画タスクからメッセージ取り出し
      if (xQueueReceive(qDispMsg, &msg, 0)==pdTRUE){
        // テキストサイズはなるべく大きめに
        int textsize = M5Cardputer.Display.height() / 25;
        if (textsize == 0) {
          textsize = 1;
        }
        M5Cardputer.Display.setTextSize(textsize);
        // カウントの表示
        if (msg.type & DISP_MSG_TIMEUPDATED_MASK){
          if (strcmp(disp_timestring, msg.timestring)!=0){
            M5Cardputer.Display.setCursor(20, 30);
            M5Cardputer.Display.println(msg.timestring);
            strcpy(disp_timestring, msg.timestring);
          }
        }
        // カーソルの表示
        if (msg.type & DISP_MSG_CARRETMOVED_MASK){
          const char* cursorbuff = (msg.carret >= CARRET_MIN && msg.carret <= CARRET_MAX) 
                             ? CURSOR_STRINGS[msg.carret] 
                             : "      ";
          
          M5Cardputer.Display.setCursor(20, 80);
          M5Cardputer.Display.println(cursorbuff);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
}
/**
 * @brief キー入力タスク
 * @param parameters Unused. Don’t care.
 */
void keyInputTask(void *parameters){
  const char lf = 0x0a;
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange()) {
      if (M5Cardputer.Keyboard.isPressed()) {
        auto keys = M5Cardputer.Keyboard.keysState();
        // キー無効フラグが立っているとき入力は使わない
        if (!keyboardEnabled) continue;
        for (auto c : keys.word) {
          xQueueSend(qKeyInput, &c, portMAX_DELAY);
        }
        if (keys.enter) {
            // OK
            xQueueSend(qKeyInput, &lf, portMAX_DELAY);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }  
}
/**
 * @brief 初期化
 */
void setup() {
  M5Cardputer.begin(true);
  // 画面表示用のキュー作成
  qDispMsg = xQueueCreate(4, sizeof(DispMsg_t));
  // キー入力用のキュー作成
  qKeyInput = xQueueCreate(4, sizeof(char));
  // ボツ音対策
  M5Cardputer.Speaker.begin();
  // 波形の初期化
  init_wave();
  // 楽譜の初期化
  init_score();
  // 画面表示タスクの起動
  xTaskCreate(displayTask,"displayTask",2048,NULL,1,NULL);
  // 制御タスクの起動
  xTaskCreate(controlTask,"controlTask",2048,NULL,1,NULL);
  // キー入力タスクの起動
  xTaskCreate(keyInputTask,"keyInputTask",2048,NULL,1,NULL);
}

/**
 * @brief arduino の loopは不使用
 */
void loop() {
}
