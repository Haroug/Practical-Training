	#if defined(WIN32) || defined(WIN64)
	#include "windows.h"
	#include "io.h"
	#else
	#include "unistd.h"
	#endif

	#include <stdlib.h>
	#include <unistd.h>
	#include <math.h>
	#include "MQTTAsync.h"
	#include "string.h"
	#include "string_util.h"
	#include <wiringPi.h>
	#include <stdio.h>
	#include <pthread.h>


	char *uri = "tcp://broker.emqx.io:1883";
	int port = 1883;

	int gQOS = 1;  //default value of qos is 1
	int keepAliveInterval = 120; //default value of keepAliveInterval is 120s
	int connectTimeout = 30; //default value of connect timeout is 30s
	int retryInterval = 10; //default value of connect retryInterval is 10s
	char *ca_path = "./conf/rootcert.pem";
	MQTTAsync client = NULL;


	#define TRY_MAX_TIME 				100   //Maximum length of attempted encryption
	#define SHA256_ENCRYPTION_LENGRH 	32
	#define TIME_STAMP_LENGTH 			10
	#define PASSWORD_ENCRYPT_LENGTH 	64

	typedef unsigned char uint8;
	typedef unsigned char u8;
	typedef unsigned int  uint16;
	typedef unsigned long uint32;

	#define HIGH_TIME 32
	char mqtt_message[1024*1024];
	char request_id[100];
	char mqtt_cmd_message[100];
	char mqtt_cmd_data[100];

	char g_message[1024];
	char g_topic[1024];     
	int pinNumber = 17;
	uint32 databuf;



	int mqttClientCreateFlag = 0; //this mqttClientCreateFlag is used to control the invocation of MQTTAsync_create, otherwise, there would be message leak.
	int retryTimes = 0;
	int minBackoff = 1000;
	int maxBackoff = 30*1000; //10 seconds
	int defaultBackoff = 1000;

	void mqtt_connect_success(void *context, MQTTAsync_successData *response) {
		retryTimes = 0;
		printf("connect success. \n");
	}

	void TimeSleep(int ms) {
	#if defined(WIN32) || defined(WIN64)
		Sleep(ms);
	#else
		usleep(ms * 1000);
	#endif
	}

	void mqtt_connect_failure(void *context, MQTTAsync_failureData *response) {
		retryTimes++;
		printf("connect failed: messageId %d, code %d, message %s\n", response->token, response->code, response->message);
		int lowBound =  defaultBackoff * 0.8;
		int highBound = defaultBackoff * 1.2;
		int randomBackOff = rand() % (highBound - lowBound + 1);
		long backOffWithJitter = (int)(pow(2.0, (double)retryTimes) - 1) * (randomBackOff + lowBound);
		long waitTImeUntilNextRetry = (int)(minBackoff + backOffWithJitter) > maxBackoff ? maxBackoff : (minBackoff + backOffWithJitter);

		TimeSleep(waitTImeUntilNextRetry);

		//connect
		int ret = mqtt_connect();
		if (ret != 0) {
			printf("connect failed, result %d\n", ret);
		}
	}

	//receive message from the server
	int mqtt_message_arrive(void *context, char *topicName, int topicLen, MQTTAsync_message *message) {
		printf( "mqtt_message_arrive() success, the topic is %s, the payload is %s \n", topicName, message->payload);
		strcpy(g_message, message->payload);
		strcpy(g_topic, topicName);
		return 1; //can not return 0 here, otherwise the message won't update or something wrong would happen
	}

	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;

	void mqtt_connection_lost(void *context, char *cause) {
		printf("mqtt_connection_lost() error, cause: %s\n", cause);
	}

	int mqtt_connect() {


		if (!mqttClientCreateFlag) {
			
			conn_opts.cleansession = 1;
			conn_opts.keepAliveInterval = keepAliveInterval;
			conn_opts.connectTimeout = connectTimeout;
			conn_opts.retryInterval = retryInterval;
			conn_opts.onSuccess = mqtt_connect_success;
			conn_opts.onFailure = mqtt_connect_failure;


			char *loginTimestamp = get_client_timestamp();
			if (loginTimestamp == NULL) {
				return -1;
			}


			int createRet = MQTTAsync_create(&client, uri, "deployment-jeff0591", MQTTCLIENT_PERSISTENCE_NONE, NULL);


			if (createRet) {
				printf("mqtt_connect() MQTTAsync_create error, result %d\n", createRet);
			} else {
				mqttClientCreateFlag = 1;
				printf("mqtt_connect() mqttClientCreateFlag = 1.\n");
			}

			MQTTAsync_setCallbacks(client, NULL, mqtt_connection_lost, mqtt_message_arrive, NULL);

		}
		printf("begin to connect the server.\n");
		int ret = MQTTAsync_connect(client, &conn_opts);
		if (ret) {
			printf("mqtt_connect() error, result %d\n", ret);
			return -1;
		}



		return 0;

	}

	void publish_success(void *context, MQTTAsync_successData *response) {
		printf("publish success, the messageId is %d \n", response ? response->token : -1);
	}

	void publish_failure(void *context, MQTTAsync_failureData *response) {
		printf("publish failure\n");
		if(response) {
			printf("publish_failure(), messageId is %d, code is %d, message is %s\n", response->token, response->code, response->message);
		}
	}

	int mqtt_publish(const char *topic, char *payload) {

		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
		MQTTAsync_message pubmsg = MQTTAsync_message_initializer;

		opts.onSuccess = publish_success;
		opts.onFailure = publish_failure;

		pubmsg.payload = payload;
		pubmsg.payloadlen = (int) strlen(payload);
		pubmsg.qos = gQOS;
		pubmsg.retained = 0;

		int ret = MQTTAsync_sendMessage(client, topic, &pubmsg, &opts);
		if (ret != 0) {
			printf( "mqtt_publish() error, publish result %d\n", ret);
			return -1;
		}

		printf("mqtt_publish(), the payload is %s, the topic is %s \n", payload, topic);
		return opts.token;
	}

	void subscribe_success(void *context, MQTTAsync_successData *response) {
		printf("subscribe success, the messageId is %d \n", response ? response->token : -1);
	}

	void subscribe_failure(void *context, MQTTAsync_failureData *response) {
		printf("subscribe failure\n");
		if(response) {
			printf("subscribe_failure(), messageId is %d, code is %d, message is %s\n", response->token, response->code, response->message);
		}
	}

	int mqtt_subscribe(const char *topic) {

		MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;

		opts.onSuccess = subscribe_success;
		opts.onFailure = subscribe_failure;

		int qos = 1;
		int ret = MQTTAsync_subscribe(client, topic, qos, &opts); //this qos must be 1, otherwise if subscribe failed, the downlink message cannot arrive.

		if (MQTTASYNC_SUCCESS != ret) {
			printf("mqtt_subscribe() error, subscribe failed, ret code %d, topic %s\n", ret, topic);
			return -1;
		}

		printf("mqtt_subscribe(), topic %s, messageId %d\n", topic, opts.token);

		return opts.token;
	}

	void time_sleep(int ms) {
	#if defined(WIN32) || defined(WIN64)
		Sleep(ms);
	#else
		usleep(ms * 1000);
	#endif
	}

	void *pth_work_func(void *arg)
	{
		char buff[1024];
		char buff2[1024];
		int size=0;
		int i=0;
		
		while(1)
		{		
			int ret = mqtt_subscribe("data/rec");

			if (ret < 0) {
				printf("subscribe topic error, result %d\n", ret);
			}
				strcpy(buff, g_message);
				strcpy(buff2, g_topic);
				size= strlen(g_message);
				memset(g_message, 0, sizeof(g_message));
				memset(g_topic, 0, sizeof(g_message));
				printf("size=%d\r\n",size);
				if(size<0){
					sleep(3);
					continue;
				}
				if(size>0)
				{
					mqtt_publish("data/rec","Accepted successfully");
					printf("TOPIC IS:%s\r\n",buff2);
					printf("DATA IS:%s\r\n",buff);
					 
					if(strstr((char*)&buff[0],"\"LED1\":1")) 
					{
						digitalWrite(23,HIGH); 
					}
					if(strstr((char*)&buff[0],"\"LED1\":0")) 
					{
						digitalWrite(23,LOW);	
					} 
					if(strstr((char*)&buff[0],"\"LED2\":1")) 
					{
						digitalWrite(24,HIGH);	
					} 
					if(strstr((char*)&buff[0],"\"LED2\":0")) 
					{
						digitalWrite(24,LOW);	
					} 
					if(strstr((char*)&buff[0],"\"LED3\":1")) 
					{
						digitalWrite(25,HIGH);
					} 
					if(strstr((char*)&buff[0],"\"LED3\":0")) 
					{
						digitalWrite(25,LOW);	
					} 
				}			
				sleep(2);	
		}
			
			printf("\r\n");
		}
	

	uint8 readSensorData(void)
	{
		uint8 crc; 
		uint8 i;
	
		pinMode(pinNumber, OUTPUT); // set mode to output
		digitalWrite(pinNumber, 0); // output a high level 
		delay(25);
		digitalWrite(pinNumber, 1); // output a low level 
		pinMode(pinNumber, INPUT); // set mode to input
		pullUpDnControl(pinNumber, PUD_UP);
	
		delayMicroseconds(27);
		if (digitalRead(pinNumber) == 0) //SENSOR ANS
		{
			while (!digitalRead(pinNumber))
				; //wait to high
	
			for (i = 0; i < 32; i++)
			{
				while (digitalRead(pinNumber))
					; //data clock start
				while (!digitalRead(pinNumber))
					; //data start
				delayMicroseconds(HIGH_TIME);
				databuf *= 2;
				if (digitalRead(pinNumber) == 1) //1
				{
					databuf++;
				}
			}
	
			for (i = 0; i < 8; i++)
			{
				while (digitalRead(pinNumber))
					; //data clock start
				while (!digitalRead(pinNumber))
					; //data start
				delayMicroseconds(HIGH_TIME);
				crc *= 2;  
				if (digitalRead(pinNumber) == 1) //1
				{
					crc++;
				}
			}
			return 1;
		}
		else
		{
			return 0;
		}
	}

	unsigned int DHT11_T;
	unsigned int DHT11_H;
	int MQ2;
	int water;
	int flame;
	int light;
	int LED1;
	int LED2;
	int LED3;

	int main(void) {

		wiringPiSetupGpio();  
		
		pinMode(23,OUTPUT);
		pinMode(24,OUTPUT);
		pinMode(25,OUTPUT);
		pinMode(6,OUTPUT);
		pinMode(22,OUTPUT);
		
		pinMode(4,INPUT);
		pinMode(18,INPUT);
		pinMode(27,INPUT);
		pinMode(5,INPUT);

		pinMode(pinNumber, OUTPUT); // set mode to output
		digitalWrite(pinNumber, 1); // output a high level 
		//connect
		int ret = mqtt_connect();
		if (ret != 0) {
			printf("connect failed, result %d\n", ret);
		}

		time_sleep(3000);
		//subscribe
		pthread_t id;
		pthread_create(&id, NULL,pth_work_func,NULL);
		pthread_detach(id);
	while(1)
		{

		pinMode(pinNumber, OUTPUT); // set mode to output
		digitalWrite(pinNumber, 1); // output a high level 
		delay(3000);
		if (readSensorData())
		{
			printf("DHT11 Sensor data read ok!\n");
			printf("RH:%d.%d\n", (databuf >> 24) & 0xff, (databuf >> 16) & 0xff); 
			printf("TMP:%d.%d\n", (databuf >> 8) & 0xff, databuf & 0xff);
			
		
			DHT11_H=((databuf >> 24) & 0xff);
			printf("DHT11_H:%d\r\n",DHT11_H);
	
			DHT11_T=((databuf >> 8) & 0xff);
			printf("DHT11_T:%d\r\n",DHT11_T);
			
			databuf = 0;
			
		}
		else
		{
			printf("Sensor dosent ans!\n");
			databuf = 0;
		}

		MQ2=digitalRead (4); 
		printf("MQ2:%d\r\n",MQ2);
		if(MQ2==0)
		{
			digitalWrite(22,HIGH); 
		}
		else
		{
			digitalWrite(22,LOW);  
		}
		

		flame=digitalRead (18); 
		printf("flame:%d\r\n",flame);


		light=digitalRead (27); 
		printf("light:%d\r\n",light);
		

		water=digitalRead (5); 
		printf("water:%d\r\n",water);


		LED1=digitalRead (23); 
		printf("LED1:%d\r\n",LED1);
		

		LED2=digitalRead (24); 
		printf("LED2:%d\r\n",LED2);


		LED3=digitalRead (25); 
		printf("LED3:%d\r\n",LED3);
		
		sprintf(mqtt_message,"{\"DHT11_T\":%d,\"DHT11_H\":%d,\"MQ2\":%d,\"water\":%d,\"flame\":%d,\"light\":%d,\"LED1\":%d,\"LED2\":%d,\"LED3\":%d}",DHT11_T,DHT11_H,MQ2,water,flame,light,LED1,LED2,LED3);


		time_sleep(1000);
		//publish data
		ret = mqtt_publish("date/pub", mqtt_message);


		if (ret < 0) {
			printf("publish data error, result %d\n", ret);
		}

		sleep(2);
		}
		return 0;
	}
