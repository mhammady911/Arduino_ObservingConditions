/*
 * 
 Arduino based weather station implementing Ascom Observing Conditions
 
 Based on Arduino Uno, Sparkfun weather shield https://www.sparkfun.com/products/12081
 and Sparkfun weather meters: https://www.sparkfun.com/products/8942
 Also using Melexis 90614 IR Sensor for sky temperature and cloud detection

 Original sketch based on what was written by Nathan Seidle https://github.com/sparkfun/Weather_Shield

 License: This code is public domain. Beers and a thank you are always welcome. 
 
 */

#include <Wire.h> //I2C needed for sensors
#include <SparkFunMPL3115A2.h> //Pressure sensor - Search "SparkFun MPL3115" and install from Library Manager
#include <SparkFunHTU21D.h> //Humidity sensor - Search "SparkFun HTU21D" and install from Library Manager
#include <SparkFunMLX90614.h> // MLX90614 IR thermometer library
#include <SerialCommand.h> // Serial command library

MPL3115A2 myPressure; //Create an instance of the pressure sensor
HTU21D myHumidity; //Create an instance of the humidity sensor
IRTherm myIRSkyTemp; // Create an IRTherm object called temp
SerialCommand myDeviceCmd; // Create a serial command object to deal with Ascom Driver

//Hardware pin definitions
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
// digital I/O pins
const byte WSPEED = 3;
const byte RAIN = 2;
const byte STAT1 = 7;
const byte STAT2 = 8;

// analog I/O pins
const byte REFERENCE_3V3 = A3;
const byte LIGHT = A1;
const byte BATT = A2;
const byte WDIR = A0;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

//Global Variables
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
long lastSecond; //The millis counter to see when a second rolls by
byte seconds; //When it hits 60, increase the current minute
byte minutes; //Keeps track of where we are in various arrays of data

long          lastWindCheck = 0;
long          windGustTimeStamp = 0;
volatile long lastWindIRQ = 0;
volatile byte windClicks = 0;

long          lastWindGustCheck = 0; // Used to calculate wind gust over 3 seconds 
byte          windGustClicks = 0; // Used to calculate wind gust over 3 seconds 
byte          currentWindGust = 0; // Used to find max wind gust over 2 minutes

volatile float rainHour[60]; //60 floating numbers to keep track of 60 minutes of rain

//These are all the weather values that wunderground expects:
int            winddir = 0; // [0-360 instantaneous wind direction]
float          windspeed = 0; // [m/s instantaneous wind speed]
float          windgust_2m[40]; // [m/s current wind gust. Collect over 2m in a rolling buffer]

float          humidity = 0; // [%]
float          pressure = 0;

float          pTempc = 0; // [Ambient temperature C, from pressure sensor]
float          hTempc = 0; // [Ambient temperature C from humidity sensor]
float          irTempc = 0; // [Ambient temperature C from IR sensor]
float          irSkyTempc = 0; // [Sky temperature C from IR sensor]
float          rainin = 0; // [rain inches over the past hour)] -- the accumulated rainfall in the past 60 min

float batt_lvl = 11.8; //[analog value from 0 to 1023]
float light_lvl = 455; //[analog value from 0 to 1023]

// volatiles are subject to modification by IRQs
volatile unsigned long raintime, rainlast, raininterval, rain;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

//Interrupt routines (these are called by the hardware interrupts, not by the main code)
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
void rainIRQ()
// Count rain gauge bucket tips as they occur
// Activated by the magnet and reed switch in the rain gauge, attached to input D2
{
  raintime = millis(); // grab current time
  raininterval = raintime - rainlast; // calculate interval between this and last event

  if (raininterval > 10) // ignore switch-bounce glitches less than 10mS after initial edge
  {
    rainHour[minutes] += 0.011; //Increase this minute's amount of rain
    rainlast = raintime; // set up for next event
  }
}

void wspeedIRQ()
// Activated by the magnet in the anemometer (2 ticks per rotation), attached to input D3
{
  if (millis() - lastWindIRQ > 10) // Ignore switch-bounce glitches less than 10ms (142MPH max reading) after the reed switch closes
  {
    lastWindIRQ = millis(); //Grab the current time
    windClicks++; //There is 1.492MPH for each click per second.
  }
}


void setup()
{
  Serial.begin(9600);

  pinMode(STAT1, OUTPUT); //Status LED Blue
  pinMode(STAT2, OUTPUT); //Status LED Green

  pinMode(WSPEED, INPUT_PULLUP); // input from wind meters windspeed sensor
  pinMode(RAIN, INPUT_PULLUP); // input from wind meters rain gauge sensor

  pinMode(REFERENCE_3V3, INPUT);
  pinMode(LIGHT, INPUT);

  //Configure the pressure sensor
  myPressure.begin(); // Get sensor online
  myPressure.setModeBarometer(); // Measure pressure in Pascals from 20 to 110 kPa
  myPressure.setOversampleRate(7); // Set Oversample to the recommended 128
  myPressure.enableEventFlags(); // Enable all three pressure and temp event flags

  //Configure the humidity sensor
  myHumidity.begin();

  seconds = 0;
  lastSecond = millis();

  // attach external interrupt pins to IRQ functions
  attachInterrupt(0, rainIRQ, FALLING);
  attachInterrupt(1, wspeedIRQ, FALLING);

  myIRSkyTemp.begin(); // Initialize I2C library and the MLX90614
  myIRSkyTemp.setUnit(TEMP_C); // Set units to Farenheit (alternatively TEMP_C or TEMP_K)

  myDeviceCmd.addCommand("Humidity", Humidity); // Atmospheric humidity (%)
  myDeviceCmd.addCommand("Pressure", Pressure); // Atmospheric presure at the observatory (Ascom needs hPa)
                                       // This must be the pressure at the observatory altitude and not the adjusted pressure at sea level. 
  myDeviceCmd.addCommand("RainRate", RainRate); // Rain rate (Ascom needs mm/hour)
                                       // This property can be interpreted as 0.0 = Dry any positive nonzero value = wet.
                                       //   Rainfall intensity is classified according to the rate of precipitation:
                                       //   Light rain — when the precipitation rate is < 2.5 mm (0.098 in) per hour
                                       //   Moderate rain — when the precipitation rate is between 2.5 mm (0.098 in) - 7.6 mm (0.30 in) or 10 mm (0.39 in) per hour
                                       //   Heavy rain — when the precipitation rate is > 7.6 mm (0.30 in) per hour, or between 10 mm (0.39 in) and 50 mm (2.0 in) per hour
                                       //   Violent rain — when the precipitation rate is > 50 mm (2.0 in) per hour
  myDeviceCmd.addCommand("SkyBrightness", SkyBrightness); // Sky brightness (Ascom needs Lux, but we are returning voltage. Ascom driver will have to be calibrated)
                                                 // 0.0001 lux  Moonless, overcast night sky (starlight)
                                                 // 0.002 lux Moonless clear night sky with airglow
                                                 // 0.27–1.0 lux  Full moon on a clear night
                                                 // 3.4 lux Dark limit of civil twilight under a clear sky
                                                 // 50 lux  Family living room lights (Australia, 1998)
                                                 // 80 lux  Office building hallway/toilet lighting
                                                 // 100 lux Very dark overcast day
                                                 // 320–500 lux Office lighting
                                                 // 400 lux Sunrise or sunset on a clear day.
                                                 // 1000 lux  Overcast day; typical TV studio lighting
                                                 // 10000–25000 lux Full daylight (not direct sun)
                                                 // 32000–100000 lux  Direct sunlight
  myDeviceCmd.addCommand("SkyTemperature", SkyTemperature); // Sky temperature in °C
  myDeviceCmd.addCommand("Temperature", Temperature); // Temperature in °C
  myDeviceCmd.addCommand("WindDirection", WindDirection); // Wind direction (degrees, 0..360.0) 
                                                 // Value of 0.0 is returned when the wind speed is 0.0. 
                                                 // Wind direction is measured clockwise from north, through east, 
                                                 // where East=90.0, South=180.0, West=270.0 and North=360.0
  myDeviceCmd.addCommand("WindGust", WindGust); // Wind gust (Ascom needs m/s) Peak 3 second wind speed over the last 2 minutes
  myDeviceCmd.addCommand("WindSpeed", WindSpeed); // Wind speed (Ascom needs m/s)

  myDeviceCmd.addCommand("WeatherDebug", printWeather); // Wind speed (Ascom needs m/s)

  // turn on interrupts
  interrupts();

  Serial.println("Observatory Weather Station online!");
  
}

void Humidity()
{
  Serial.println(humidity,5);
}

void Pressure()
{
  Serial.println(pressure,5);
}

void RainRate()
{
  float rainRate = rainin * 25.4;
  Serial.println(rainRate,5);
}

void SkyBrightness()
{
  Serial.println(get_light_level(),5);
}

void SkyTemperature()
{
  Serial.println(irSkyTempc,5);
}

void Temperature()
{
  Serial.println(irTempc,5);
}

void WindSpeed()
{
  Serial.println(windspeed,5);
}

void WindGust()
{
  float windgust = 0;
  for (int i = 0; i < 40; i++)
    if (windgust_2m[i] > windgust)
      windgust = windgust_2m[i];
      
  Serial.println(windgust,5);
}

void WindDirection()
{
  Serial.println(winddir,5);
}

void loop()
{
  //Keep track of which minute it is
  if(millis() - lastSecond >= 1000)
  {
    digitalWrite(STAT1, HIGH); //Blink stat LED

    lastSecond += 1000;

    // Calculate all weather readings
    calcWeather();

    digitalWrite(STAT1, LOW); //Turn off stat LED
  }
}

//Calculates each of the variables that wunderground is expecting
void calcWeather()
{
  //Calc humidity
  humidity = myHumidity.readHumidity();

  //Calc pressure
  pressure = myPressure.readPressure();

  // Calc rain
  calc_rain();

  //Calc light level
  light_lvl = get_light_level();

  // Calc Temp
  // Ambient from humidity sensor
  hTempc = myHumidity.readTemperature();
  // Ambient from pressure sensor
  pTempc = myPressure.readTemp();
  // Ambient and sky temp from IR sensor
  if (myIRSkyTemp.read()) // Read from the sensor
  { // If the read is successful:
    float irTempc = myIRSkyTemp.ambient(); // Get updated ambient temperature
    float irSkyTempc = myIRSkyTemp.object(); // Get updated object temperature
  }

    // Calc Wind
  calc_wind();

  if(++seconds > 59)
  {
    seconds = 0;
    if(++minutes > 59) minutes = 0;
    rainHour[minutes] = 0; //Zero out this minute's rainfall amount
  }
}

void calc_rain()
{
  //Calculate amount of rainfall for the last 60 minutes
  rainin = 0;
  for(int i = 0 ; i < 60 ; i++)
    rainin += rainHour[i];
  
}

void calc_wind()
{
  // Wind Direction
  unsigned int adc;

  adc = analogRead(WDIR); // get the current reading from the sensor

  // The following table is ADC readings for the wind direction sensor output, sorted from low to high.
  // Each threshold is the midpoint between adjacent headings. The output is degrees for that ADC reading.
  // Note that these are not in compass degree order! See Weather Meters datasheet for more information.

  winddir = -1;
  if (adc < 380) winddir = 113;
  if (adc < 393) winddir = 68;
  if (adc < 414) winddir = 90;
  if (adc < 456) winddir = 158;
  if (adc < 508) winddir = 135;
  if (adc < 551) winddir = 203;
  if (adc < 615) winddir = 180;
  if (adc < 680) winddir = 23;
  if (adc < 746) winddir = 45;
  if (adc < 801) winddir = 248;
  if (adc < 833) winddir = 225;
  if (adc < 878) winddir = 338;
  if (adc < 913) winddir = 0;
  if (adc < 940) winddir = 293;
  if (adc < 967) winddir = 315;
  if (adc < 990) winddir = 270;

  // WindSpeed
  
  float deltaTime = millis() - lastWindCheck; //750ms

  deltaTime /= 1000.0; //Covert to seconds

  windspeed = (float)windClicks / deltaTime; //3 / 0.750s = 4
  windspeed *= 1.492; //4 * 1.492 = 5.968MPH
  windspeed *= 0.44704; // Convert to m/s
  lastWindCheck = millis();
  
  //WindGust
  // Calculate the average wind speed over the last 3 seconds
  float windGustDeltaTime = millis() - lastWindGustCheck;
  if (windGustDeltaTime < 3000)
  {
    windGustClicks += windClicks;
  } else {
    float windgust = (float)windGustClicks / windGustDeltaTime;

    windgust *= 1.492;
    windgust *= 0.44704;

    if (++currentWindGust > 39)
      currentWindGust = 0;

    windgust_2m[currentWindGust] = windgust;
    
    lastWindGustCheck = millis();
  }

  windClicks = 0; //Reset and start watching for new wind

}

//Returns the voltage of the light sensor based on the 3.3V rail
//This allows us to ignore what VCC might be (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
float get_light_level()
{
  float operatingVoltage = analogRead(REFERENCE_3V3);

  float lightSensor = analogRead(LIGHT);

  operatingVoltage = 3.3 / operatingVoltage; //The reference voltage is 3.3V

  lightSensor = operatingVoltage * lightSensor;

  return(lightSensor);
}

//Prints the various variables directly to the port
//I don't like the way this function is written but Arduino doesn't support floats under sprintf
void printWeather()
{
  Serial.println();

  Serial.print("$,winddir=");
  Serial.print(winddir);
}
