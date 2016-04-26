#include <Streaming.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <CButton.h>
#include <COutputPWM.h>
#include <CDoubleOutput.h>
#include <CTempDS18B20.h>

//----------------------------------------------------------------------------
enum EnumResult
{
#include "CommonErrors.h"
  Error_TempSensor_Create,
  Error_TempSensor_Read,
};

//--------------------------------------
enum EnumButton
{
  BtnNone   = 0,
  BtnMenu   = 1,
  BtnPlus   = 2,
  BtnMinus  = 3,
};

//--------------------------------------
enum EnumScreen
{
  ScrNone = 0,
  ScrHome00_Status,
  ScrHome01_TempOverview,
  ScrHome10_TempVorlauf,
  ScrHome11_TempWasser,
  ScrHome12_TempHeizung,
  ScrHome13_Temp04,
  ScrHome14_Temp05,
  ScrMenu00,
  ScrMenu01_TempWasser,
  ScrMenu02_TempHeizung,
  ScrMenu03_TempHysterese,
  ScrMenu04_MischerStellzeit,
  ScrMenu05_Pumpe,
  ScrMenu06_Sensor1Adresse,
  ScrMenu07_Sensor2Adresse,
  ScrMenu08_Sensor3Adresse,
  ScrService00,
  ScrService01_SensorSuche,
};

//--------------------------------------
enum EnumAction
{
  ActNone,
  ActSvcSensorSearchStart,
  ActSvcSensorSearchAbort,
};

//--------------------------------------
enum EnumOperation
{
  OpNone,
  Op_Trinkwasser = 1, // ab hier sortiert nach Priorität
  Op_Heizung,
  Op_Aus,
};

//----------------------------------------------------------------------------
#define MAXTEMPCOUNT  10
#define TEMPCOUNT     3
const unsigned int mc_tIntervalWatchdog = 100;
const unsigned int mc_tIntervalTempCtrl = 10000;
const unsigned int mc_tTimeoutMenu      = 60000;

const int I2C_Addr_Display    = 0x27;

const int DI_TempDS18B20      =  2;
const int DI_Button           =  3;  // für Interrupts
const int DI_ButtonSwMenu     =  4;
const int DI_ButtonRtPlus     =  5;
const int DI_ButtonBlMinus    =  6;
const int DO_Watchdog         = 13;
const int DO_Mischer1_Kreis   =  9;
const int DO_Mischer1_Heizen  =  8;
const int DO_Mischer2_Wasser  = 10;
const int DO_Mischer2_Heizung = 11;
const int DO_Pumpe            = 12;
const int DO_LED_Fehler       =  7;

//----------------------------------------------------------------------------
// settings
float               m_fTWasserSoll    = 0.0;
float               m_fTHeizungSoll   = 0.0;
float               m_fTHysterese     = 0.0;
int                 m_iMischerStellzeit = 140;
byte                m_byPumpeLaufzeit = 0;  // [%]
byte                m_aabyTempDS18B20Addr[TEMPCOUNT][8];

//----------------------------------------------------------------------------
EnumResult          m_enResult        = EnumResult::InProgress;
EnumOperation       m_enOpStatus      = EnumOperation::OpNone;
EnumOperation       m_enOpRequest     = EnumOperation::OpNone;
unsigned long       m_tWatchdog       = 0;
unsigned long       m_tTempCtrl       = 0;
unsigned long       m_tLastMenuChange = 0;
byte                m_byTempCtrlError = 0;
EnumScreen          m_enScreen        = EnumScreen::ScrHome00_Status;
EnumButton          m_enLastButton    = EnumButton::BtnNone;
LiquidCrystal_I2C   m_oDisplay (I2C_Addr_Display, 16, 2);
OneWire             m_oOneWire (DI_TempDS18B20);
CTempDS18B20*       m_aoTempSensor[TEMPCOUNT];
COutputPWM          m_oPumpe (DO_Pumpe, false, 0, 100);
CDoubleOutput       m_oMischer1KH (DO_Mischer1_Kreis,  DO_Mischer1_Heizen,  false, m_iMischerStellzeit * 1000, m_iMischerStellzeit * 1000);
CDoubleOutput       m_oMischer2WH (DO_Mischer2_Wasser, DO_Mischer2_Heizung, false, m_iMischerStellzeit * 1000, m_iMischerStellzeit * 1000);
float               m_fTVorlauf       = 0.0;
float               m_fTWasser        = 0.0;
float               m_fTHeizung       = 0.0;
CButton m_oButtonSwMenu  (DI_ButtonSwMenu , 5, false, false);
CButton m_oButtonRtPlus  (DI_ButtonRtPlus , 5, false, false);
CButton m_oButtonBlMinus (DI_ButtonBlMinus, 5, false, false);

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
void DisplayPrintRow (byte                       i_byRow,
                      const __FlashStringHelper* i_poLine)
{
  if (i_poLine && i_byRow >= 1 && i_byRow <= 2)
  {
    m_oDisplay.setCursor(i_byRow - 1, 0);
    m_oDisplay << i_poLine << endl;
  }
}

//------------------------------------------------------------------------------
void DisplayPrint (const __FlashStringHelper* i_poLine1,
                   const __FlashStringHelper* i_poLine2)
{
  m_oDisplay.clear ();
  DisplayPrintRow (1, i_poLine1);
  DisplayPrintRow (2, i_poLine2);
}

//------------------------------------------------------------------------------
void setup() 
{
  Serial.begin (9600);
    
  //--------------------------------------
  pinMode (DI_Button, INPUT);
  
  pinMode (DO_Watchdog, OUTPUT);
  pinMode (DO_LED_Fehler, OUTPUT);

  // attachInterrupt (digitalPinToInterrupt(DI_Button), ButtonLesen, RISING);

  //--------------------------------------
  m_oDisplay.init();
  m_oDisplay.backlight();

  //--------------------------------------
  m_enResult = SettingsRead ();
  if (m_enResult != EnumResult::SUCCESS) return;
  
  //--------------------------------------
  for (int ixCnt = 0; ixCnt < TEMPCOUNT; ixCnt++)
  {
    CTempDS18B20::EnumResult enResultDS18B20 = CTempDS18B20::EnumResult::InProgress;
    m_aoTempSensor[ixCnt] = new CTempDS18B20 (&m_oOneWire, m_aabyTempDS18B20Addr[ixCnt], enResultDS18B20);
    if (enResultDS18B20 != CTempDS18B20::EnumResult::SUCCESS)
      m_enResult = EnumResult::Error_TempSensor_Create;
    Serial << F("new Temp sensor DS18B20, result=") << enResultDS18B20 << endl;
  }
  if (m_enResult != EnumResult::SUCCESS) return;

  //--------------------------------------
  DisplayPrint (F("Initialisierung"), F("abgeschlossen."));
  m_enResult = EnumResult::InProgress;
  delay(5000);
  m_oDisplay.clear ();
  Serial << F("setup completed.") << endl;
}

//------------------------------------------------------------------------------
void loop() 
{
  EnumResult        enResult        = EnumResult::InProgress;
  unsigned long     tTime           = millis ();
  static bool       s_bWatchdog     = false;
  
  // Watchdog
  if ((m_enResult == EnumResult::InProgress && tTime - m_tWatchdog > mc_tIntervalWatchdog)
  ||  (m_enResult != EnumResult::InProgress && tTime - m_tWatchdog > mc_tIntervalWatchdog * 3))
  {
    m_tWatchdog = tTime;
    s_bWatchdog = !s_bWatchdog;
    digitalWrite (DO_Watchdog, s_bWatchdog);
  }

  if (tTime - m_tTempCtrl > mc_tIntervalTempCtrl)
  {
    enResult = TemperatureControl ();
    if (enResult == EnumResult::SUCCESS)
      m_tTempCtrl = tTime;      
    else if (enResult != EnumResult::InProgress)
      m_enResult = enResult;
  }
  
  if (m_enResult != EnumResult::InProgress)
  {
    Serial << F("Error=") << m_enResult << endl;
  }
  
  EnumButton enButton = EnumButton::BtnNone;
  ButtonRead (enButton);
  if (enButton == m_enLastButton)
    enButton = EnumButton::BtnNone;
  m_enLastButton = enButton;
  
  UpdateUI (enButton);

  delay (10);
}

//------------------------------------------------------------------------------
EnumResult TemperatureControl ()
{
  EnumResult                enResult        =               EnumResult::InProgress;
  CTempDS18B20::EnumResult  enResultDS18B20 = CTempDS18B20::EnumResult::InProgress;

  bool              bNewRequest = false;
  static byte       s_byxSensor = 0;
  static byte       s_byError   = 0;
  static EnumResult s_enResult  = EnumResult::InProgress;
  
  Serial << F("TemperatureControl") << endl;
  if (s_byxSensor < TEMPCOUNT)
  {
    float fTemp = 0.0;
    enResultDS18B20 = m_aoTempSensor[s_byxSensor]->ReadTemp (false, fTemp);
    if (enResultDS18B20 == CTempDS18B20::EnumResult::SUCCESS)
    {
      s_byxSensor++;
      if (s_byxSensor == 0) m_fTVorlauf = fTemp;
      if (s_byxSensor == 1) m_fTWasser  = fTemp;
      if (s_byxSensor == 2) m_fTHeizung = fTemp;
    }
    else if (enResultDS18B20 != CTempDS18B20::EnumResult::InProgress)
      s_enResult = EnumResult::Error_TempSensor_Read;
  }
  if (s_byxSensor >= TEMPCOUNT)
  {
    s_byxSensor = 0;
    if (s_enResult != EnumResult::InProgress)
      s_byError++;
    if (s_byError > 10) return s_enResult;
    s_enResult = EnumResult::InProgress;
    
    if (m_fTHysterese <  1.0) m_fTHysterese =  1.0;
    if (m_fTHysterese > 10.0) m_fTHysterese = 10.0;
    bool bVersorgung = m_fTVorlauf > m_fTWasser || m_fTVorlauf > m_fTHeizung;

    Serial << F("TV=") << m_fTVorlauf << F(", TW=") << m_fTWasser << F(", TH=") << m_fTHeizung << endl;
    Serial << F("alt: Status=") << m_enOpStatus << ", Anforderung=" << m_enOpRequest << endl;
    if (m_enOpRequest == EnumOperation::OpNone)
    {      
      // Prüfung Anforderung Trinkwasser
      if (m_enOpStatus == EnumOperation::Op_Trinkwasser && m_fTWasser < m_fTWasserSoll)
        m_enOpStatus = EnumOperation::Op_Aus;
      if (m_enOpStatus > EnumOperation::Op_Trinkwasser && m_fTWasser > m_fTWasserSoll + m_fTHysterese)
        m_enOpStatus = EnumOperation::Op_Trinkwasser;
  
      // Prüfung Anforderung Heizung
      if (m_enOpStatus == EnumOperation::Op_Heizung && m_fTHeizung < m_fTHeizungSoll)
        m_enOpStatus = EnumOperation::Op_Aus;
      if (m_enOpStatus > EnumOperation::Op_Heizung && m_fTHeizung > m_fTHeizungSoll + m_fTHysterese)
        m_enOpStatus = EnumOperation::Op_Heizung;

      // Prüfung Versorgung
      if (bVersorgung)
        m_enOpRequest = m_enOpStatus;
      else
        m_enOpRequest = EnumOperation::Op_Aus;
      bNewRequest = true;
    }

    Serial << F("neu: Status=") << m_enOpStatus << ", Anforderung=" << m_enOpRequest << endl;

    m_oMischer1KH.Write ();
    m_oMischer2WH.Write ();
      
    if (m_enOpRequest != EnumOperation::OpNone && bNewRequest)
    {
      if (m_enOpRequest == EnumOperation::Op_Aus)
        m_oPumpe.Set (m_byPumpeLaufzeit, 100);
      else
        m_oPumpe.Set (100, 100);

      if (m_enOpRequest == EnumOperation::Op_Aus)
        m_oMischer1KH.Write (HIGH, LOW, true);
      else
        m_oMischer1KH.Write (LOW, HIGH, true);
   
      if (m_enOpRequest == EnumOperation::Op_Trinkwasser)
        m_oMischer2WH.Write (HIGH, LOW, true);
      else if (m_enOpRequest == EnumOperation::Op_Heizung)
        m_oMischer2WH.Write (LOW, HIGH, true);
    }

    if (!m_oMischer1KH.IsActive() && !m_oMischer2WH.IsActive())
      m_enOpRequest = EnumOperation::OpNone;

    m_oPumpe.Write ();
    
    enResult = EnumResult::SUCCESS;
  }

  return enResult;
};

//------------------------------------------------------------------------------
void ButtonRead (EnumButton &o_enButton)
{
  bool bButtonSwMenu  = m_oButtonSwMenu .Read();
  bool bButtonRtPlus  = m_oButtonRtPlus .Read();
  bool bButtonBlMinus = m_oButtonBlMinus.Read();

  o_enButton = EnumButton::BtnNone;
  if (bButtonSwMenu)
    o_enButton = EnumButton::BtnMenu;
  else if (bButtonRtPlus)
    o_enButton = EnumButton::BtnPlus;
  else if (bButtonBlMinus)
    o_enButton = EnumButton::BtnMinus;
}

//------------------------------------------------------------------------------
void UpdateUI (EnumButton i_enButton)
{
  Serial << F("UpdateUI Button=") << i_enButton << endl;

  if (i_enButton != EnumButton::BtnNone)
    m_tLastMenuChange = millis ();
  else if (millis() - m_tLastMenuChange > mc_tTimeoutMenu)
    m_enScreen = EnumScreen::ScrHome00_Status;
    
  //----------------------------------------
  // navigation
  switch (m_enScreen)
  {
  case EnumScreen::ScrHome00_Status:
  case EnumScreen::ScrHome01_TempOverview:
  case EnumScreen::ScrHome10_TempVorlauf:
  case EnumScreen::ScrHome11_TempWasser:
  case EnumScreen::ScrHome12_TempHeizung:
  case EnumScreen::ScrHome13_Temp04:
  case EnumScreen::ScrHome14_Temp05:
    switch (i_enButton)
    {
    case EnumButton::BtnMenu:
      m_enScreen = EnumScreen::ScrMenu00;
      break;
    case EnumButton::BtnPlus:
      m_enScreen = (EnumScreen)(m_enScreen + 1);
      if (m_enScreen > EnumScreen::ScrHome10_TempVorlauf + TEMPCOUNT)
        m_enScreen = EnumScreen::ScrHome00_Status;
      break;
    case EnumButton::BtnMinus:
      m_enScreen = (EnumScreen)(m_enScreen - 1);
      if (m_enScreen < EnumScreen::ScrHome00_Status)
        m_enScreen = (EnumScreen)(EnumScreen::ScrHome10_TempVorlauf + TEMPCOUNT - 1);
      break;
    default:
      break;
    }  
    break;
    
  case EnumScreen::ScrMenu00:
    switch (i_enButton) {
    case EnumButton::BtnMenu:  m_enScreen = EnumScreen::ScrService00;     break;
    case EnumButton::BtnPlus:  m_enScreen = EnumScreen::ScrMenu01_TempWasser; break;
    case EnumButton::BtnMinus: m_enScreen = EnumScreen::ScrMenu08_Sensor3Adresse; break;
    default: break;
    }
    break;

  case EnumScreen::ScrMenu01_TempWasser:
  case EnumScreen::ScrMenu02_TempHeizung:
  case EnumScreen::ScrMenu03_TempHysterese:
  case EnumScreen::ScrMenu04_MischerStellzeit:
  case EnumScreen::ScrMenu05_Pumpe:
  case EnumScreen::ScrMenu06_Sensor1Adresse:
  case EnumScreen::ScrMenu07_Sensor2Adresse:
  case EnumScreen::ScrMenu08_Sensor3Adresse:
    switch (i_enButton) {
    case EnumButton::BtnMenu:
      // TODO edit
      break;
    case EnumButton::BtnPlus:
      m_enScreen = (EnumScreen)(m_enScreen + 1);
      if (m_enScreen > EnumScreen::ScrMenu08_Sensor3Adresse)
        m_enScreen = EnumScreen::ScrMenu00;
      break;
    case EnumButton::BtnMinus:
      m_enScreen = (EnumScreen)(m_enScreen - 1);
      break;
    default: break;
    }
    break;

  case EnumScreen::ScrService00:
    switch (i_enButton) {
    case EnumButton::BtnMenu:  m_enScreen = EnumScreen::ScrHome00_Status;     break;
    case EnumButton::BtnPlus:  m_enScreen = EnumScreen::ScrService01_SensorSuche; break;
    case EnumButton::BtnMinus: m_enScreen = EnumScreen::ScrService01_SensorSuche; break;
    default: break;
    }
    break;
  
  case EnumScreen::ScrService01_SensorSuche:
    switch (i_enButton) {
    case EnumButton::BtnMenu:
      // TODO edit
      break;
    case EnumButton::BtnPlus:
      m_enScreen = (EnumScreen)(m_enScreen + 1);
      if (m_enScreen > EnumScreen::ScrService01_SensorSuche)
        m_enScreen = EnumScreen::ScrService00;
      break;
    case EnumButton::BtnMinus:
      m_enScreen = (EnumScreen)(m_enScreen - 1);
      break;
    default: break;
    }
    break;

  default:
    break;
  }

  //----------------------------------------
  // view content
  EnumOperation enStatus = EnumOperation::OpNone;
  switch (m_enScreen)
  {
  case EnumScreen::ScrHome00_Status:
    if (m_enResult != EnumResult::InProgress) 
    {
      m_oDisplay.clear ();
      m_oDisplay.setCursor(0,0);
      m_oDisplay << F("Fehler ") << m_enResult;
      m_oDisplay.setCursor(1,0);
    } 
    else if (m_enOpRequest != EnumOperation::OpNone) 
    {
      DisplayPrint (F("Wechsel zu"), 0);
      enStatus = m_enOpRequest;
    }
    else 
    {
      DisplayPrint (F("Betrieb"), 0);
      enStatus = m_enOpStatus;
    }
    
    switch (enStatus)
    {
    case EnumOperation::Op_Trinkwasser:
      DisplayPrintRow (1, F("Erwärm.Trinkwass"));
      break;
    case EnumOperation::Op_Heizung:
      DisplayPrintRow (1, F("Heiz.Unterstütz."));
      break;
    case EnumOperation::Op_Aus:
      DisplayPrintRow (1, F("keine Wärmenutz."));
      break;
    default: break;
    }
    break;
  case EnumScreen::ScrHome01_TempOverview:
    DisplayPrint (F("T: Vor Wass Heiz"), 0);
    m_oDisplay.setCursor(1,0);
    m_oDisplay << String(m_fTVorlauf, 1).padLeftC(5)
               << String(m_fTWasser , 1).padLeftC(5)
               << String(m_fTHeizung, 1).padLeftC(5);
    break;
  case EnumScreen::ScrHome10_TempVorlauf:
    DisplayPrint (F("S.1 T.Vorlauf"), 0);
    m_oDisplay.setCursor(1,0);
    m_oDisplay << String(m_fTVorlauf, 1).padLeftC(5);
    break;
  case EnumScreen::ScrHome11_TempWasser:
    DisplayPrint (F("S.2 T.Wasser"), 0);
    m_oDisplay.setCursor(1,0);
    m_oDisplay << String(m_fTWasser, 1).padLeftC(5) << " (" << String(m_fTWasserSoll, 1).padLeftC(4) << ")";
    break;
  case EnumScreen::ScrHome12_TempHeizung:
    DisplayPrint (F("S.3 T.Heizung"), 0);
    m_oDisplay.setCursor(1,0);
    m_oDisplay << String(m_fTHeizung, 1).padLeftC(5) << " (" << String(m_fTHeizungSoll, 1).padLeftC(4) << ")";
    break;
  case EnumScreen::ScrHome13_Temp04:
  case EnumScreen::ScrHome14_Temp05:
    break;

  default:
    break;
  }
}

//------------------------------------------------------------------------------
EnumResult SettingsRead ()
{
  // read from EEPROM
  return EnumResult::SUCCESS;
}

//------------------------------------------------------------------------------
EnumResult SettingsWrite ()
{
  // write to EEPROM
  return EnumResult::SUCCESS;
}

//------------------------------------------------------------------------------
EnumResult Service_TempSensorSearch ()
{
  EnumResult enResult = EnumResult::InProgress;
  
  m_oOneWire.reset_search();
  DisplayPrint (F("Suche nach"), F("Temp.sensoren"));

  unsigned long tStart = millis ();
  byte byDevices = 0;
  byte abyDevAddr[8];
  bool bFound   = false;
  do
  {
    bool bSearch = false;
    EnumButton enButton = EnumButton::BtnNone;
    ButtonRead (enButton);
    if (enButton != m_enLastButton)
    {
      m_enLastButton = enButton;
      bSearch = enButton == EnumButton::BtnPlus;
      if (enButton == EnumButton::BtnMinus)
      {
        DisplayPrint (F("Abbruch durch"), F("Benutzer"));
        delay (2000);
        return EnumResult::UserAbort;
      }
    }

    if (millis() - tStart > mc_tTimeoutMenu)
    {
      DisplayPrint (F("Abbruch wegen"), F("Zeitüberschreit."));
      delay (2000);
      return EnumResult::Error_Timeout;
    }
    if (bSearch)
    {
      m_oDisplay.clear ();
      m_oDisplay.setCursor(0,0);
      bFound = m_oOneWire.search (abyDevAddr);
      tStart = millis ();
      if (bFound)
      {
        byDevices++;
        m_oDisplay << F("Sensor #") << byDevices;
        m_oDisplay.setCursor(1,0);
        byte byCRC = OneWire::crc8 (abyDevAddr, 7);
        if (byCRC != abyDevAddr[7])
          m_oDisplay << F("CRC Fehler");
        else
        {
          String sAddr;
          for (int ixCnt = 0; ixCnt < 8; ixCnt++)
            m_oDisplay << " " << (abyDevAddr[ixCnt] > 0xF ? "" : "0") << _HEX(abyDevAddr[ixCnt]);
        }
      }
    }
  }
  while (enResult == EnumResult::InProgress);

  DisplayPrint (F("Suche beendet."), 0);
  m_oDisplay.setCursor(1,0);
  m_oDisplay << byDevices << F(" Sensoren");
  delay (2000);
  return EnumResult::SUCCESS;
}
