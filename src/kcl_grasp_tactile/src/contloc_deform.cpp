/*BSD 2-Clause License

Copyright (c) 2018, Teng Sun 
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#include <cstdio>
#include <math.h>
#include <sys/time.h>
#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>

#include <ros/ros.h>
#include <ros/package.h>
#include <geometry_msgs/WrenchStamped.h>
#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <sr_grasp_msgs/KCL_ContactStateStamped.h>
#include <std_msgs/Float64.h>
#include <std_msgs/Float32.h>
#include <levmar.h>

using namespace std;

//#define MAX 1000
#define MAX 2500 //For defrom condition
#define pi 3.14159265
	//	Choose from two different methods
#define GD true
#define LM false
const std::string green("\033[1;32m");
const std::string reset("\033[0m");

double a,b,c,y00,z0,frot,sfrot,cfrot;
FILE *data;		//Pointer used to manage files

//double E = 0.15168; 
double E =0.33784310672/4; //Dragon skin 20
double deform =0;

class Contact
{
public:
	Contact();
	ros::NodeHandle n;
	ros::Subscriber sub;
	std::ofstream datfile;
	ros::Publisher KCL_ContactState_pub,KCL_ContactState_pub_ant ;
	void sensorCallback(const geometry_msgs::WrenchStamped&);
	int setFinger(char* arg);

private:
	timespec ts;
	long int start,t2;
	//	double a,b,c,y0,z0,frot,sfrot,cfrot;		//Used as private parameters
	int finger;
	string frame_id_ ="finger1";

	void magdir();
	int location();
	int location_deform();
	void Fnormtan();
	geometry_msgs::Point changeFrame(geometry_msgs::Point in_pt);
	geometry_msgs::Vector3 changeFrame(geometry_msgs::Vector3 in_v);

	double opts[LM_OPTS_SZ], info[LM_INFO_SZ];
	double angles[3],x4,netforcef;
	double ub[4],lb[4];
	int max_iter;
	bool write_to_file;
	double p[4];

	sr_grasp_msgs::KCL_ContactStateStamped ContactState,ContactState_ant;
	geometry_msgs::WrenchStamped ftsensor;
	geometry_msgs::Point cl;
	geometry_msgs::Vector3 Fn,Ft,ltorque,normal;
	double Fnormal,Ltorque;
};

Contact::Contact()
{
	//ros::NodeHandle n("finger");
	//sub=n.subscribe("netft_data", 100, &Contact::sensorCallback,this);		//Used for netbox nano17 sensor
	sub=n.subscribe("/finger1/nano17ft", 100, &Contact::sensorCallback,this);
	KCL_ContactState_pub_ant=n.advertise<sr_grasp_msgs::KCL_ContactStateStamped>("ContactState",10);
	KCL_ContactState_pub=n.advertise<sr_grasp_msgs::KCL_ContactStateStamped>("ContactState_new",10);
	string tmp_frame_id=n.getNamespace();		//Assume the namespace has the same name as the frameid

	//	If use ROS parameter function
	n.getParam("frame", frame_id_);
	ROS_WARN("Frame: ", frame_id_.c_str());
	n.getParam("contact/a", a);
	n.getParam("contact/b", b);
	n.getParam("contact/c", c);
	n.getParam("contact/y00", y00);
	n.getParam("contact/z0", z0);
	n.getParam("contact/frot", frot);
	n.getParam("contact/lm/max_iter", max_iter);
	n.getParam("contact/lm/tau",opts[0]);
	n.getParam("contact/lm/e1", opts[1]);
	n.getParam("contact/lm/e2", opts[2]);
	n.getParam("contact/lm/e3", opts[3]);
	n.getParam("contact/lm/write_to_file", write_to_file);

	//	If decide the parameters manually
	//!!---The new fingertip with maxon motors 
	//a =8.5; b=16; c=9.5; y00 =-1.0; z0 = 0.0; frot = -138.0/180*pi;
	//!!---Old version fingertip Sun---!!//
	a = 17.88; b=17.88; c = 17.88; y00 = 0; z0 = 0; frot = -138.0/180*pi;  
//	a = 17.88; b=17.88; c = 15.5; y00 = 0; z0 = 0; frot = -48.0/180*pi;  
	//!!---End of old version fingertip Sun---!!//
	max_iter = 100;
	opts[0] = 1e-3; opts[1] = 1e-4;opts[2] = 1e-4;opts[3] = 1e-4; 
	write_to_file = false;
	lb[0]=-a;lb[1]=-b+y00+6;lb[2]=-4;lb[3]=-20;
	ub[0]=a;ub[1]=b+y00;ub[2]=c;ub[3]=20;
	sfrot=sin(frot);
	cfrot=cos(frot);
    
	//Options for:  ret=dlevmar_der(ForceBal2,JacForceBal2, p, x, 4, 6, 1000, opts, info, NULL, NULL, &ftsensor);[\tau, \epsilon1, \epsilon2, \epsilon3]
	//opts[0]=LM_INIT_MU; opts[1]=1E-15; opts[2]=1E-15; opts[3]=1E-20;
	//!!Used to write contact location data to files
	if(write_to_file)
	{
		datfile.open("contacts.txt",ios::app);
		datfile.precision(9);
	}
}

void Contact::Fnormtan()
{
	//	Surface normal = \/S=(dS/dx,dS/dy,dS/dz)
	//	Normal.header.stamp = ros::Time::now();
	normal.x=2/(a*a)*cl.x;
	normal.y=2/(b*b)*(cl.y-y00);
	normal.z=2/(c*c)*(cl.z-z0);
	double normal_norm=sqrt(normal.x*normal.x
                            +normal.y*normal.y
                            +normal.z*normal.z);
	
	double Q;
	Q=(normal.x*ftsensor.wrench.force.x
			+normal.y*ftsensor.wrench.force.y
			+normal.z*ftsensor.wrench.force.z)
			/(normal_norm*normal_norm);
	Fnormal=Q*normal_norm;
	Fn.x=Q*normal.x;
	Fn.y=Q*normal.y;
	Fn.z=Q*normal.z;

	Ft.x=ftsensor.wrench.force.x-Fn.x;
	Ft.y=ftsensor.wrench.force.y-Fn.y;
	Ft.z=ftsensor.wrench.force.z-Fn.z;

	ltorque.x=x4*normal.x;
	ltorque.y=x4*normal.y;
	ltorque.z=x4*normal.z;
	Ltorque=x4*normal_norm;

	//	Normalize normal vector
	normal.x/=normal_norm;
	normal.y/=normal_norm;
	normal.z/=normal_norm;
}

void Contact::magdir()
{
	double fx=ftsensor.wrench.force.x;
	double fy=ftsensor.wrench.force.y;
	double fz=ftsensor.wrench.force.z;

	netforcef=sqrt(fx*fx+fy*fy+fz*fz);
	angles[0]=acos(fx/netforcef);
	angles[1]=acos(fy/netforcef);
	angles[2]=acos(fz/netforcef);
}

void ForceBal2(double *p, double *g, int m, int nn, void *data)
{
	geometry_msgs::WrenchStamped ft;
	memcpy(&ft,data,sizeof(geometry_msgs::WrenchStamped));

	g[0]=p[3]*((2*p[0])/(a*a))-p[2]*ft.wrench.force.y+p[1]*ft.wrench.force.z-ft.wrench.torque.x;
	g[1]=p[3]*((2*(p[1]-y00))/(b*b))-p[0]*ft.wrench.force.z+p[2]*ft.wrench.force.x-ft.wrench.torque.y;
	g[2]=p[3]*((2*(p[2]-z0))/(c*c))-p[1]*ft.wrench.force.x+p[0]*ft.wrench.force.y-ft.wrench.torque.z;
	g[3]=(p[0]/a)*(p[0]/a) + ((p[1]-y00)/b)*((p[1]-y00)/b) + ((p[2]-z0)/c)*((p[2]-z0)/c) - 1;
}

void JacForceBal2(double *p, double *jac, int m, int nn, void *data)
{
	geometry_msgs::WrenchStamped ft;
	memcpy(&ft,data,sizeof(geometry_msgs::WrenchStamped));

	register int i=0;
	jac[i++]=p[3]*2/(a*a);
	jac[i++]=ft.wrench.force.z;
	jac[i++]= -ft.wrench.force.y;
	jac[i++]=(2*p[0])/(a*a);

	jac[i++]= -ft.wrench.force.z;
	jac[i++]=p[3]*2/(b*b);
	jac[i++]=ft.wrench.force.x;
	jac[i++]=(2*(p[1]-y00))/(b*b);

	jac[i++]=ft.wrench.force.y;
	jac[i++] = -ft.wrench.force.x;
	jac[i++]=p[3]*2/(c*c);
	jac[i++]=(2*(p[2]-z0))/(c*c);

	jac[i++]=(2*p[0])/(a*a);
	jac[i++]=(2*(p[1]-y00))/(b*b);
	jac[i++]=(2*(p[2]-z0))/(c*c);
	jac[i++]=0;
}

int Contact::location()
{
	float c1,c2,c3,c4;
	float converg;

	double fx=ftsensor.wrench.force.x;
	double fy=ftsensor.wrench.force.y;
	double fz=ftsensor.wrench.force.z;

	double mx=ftsensor.wrench.torque.x;
	double my=ftsensor.wrench.torque.y;
	double mz=ftsensor.wrench.torque.z;

	float x[4]={0,0,0,0};
	int k=0;
	float g[4]={1,1,1,1};
	float G[4][4];

	if(finger==2 && (fz>0.1 || (fy<-0.1 && fy/netforcef<-0.5))){
		c1=0;
		c2=b+y00;
		c3=0;
		c4=0;
	}
	else{
		c1=0;
		c2=0;
		c3=c+z0;
		c4=0;
	}
	converg=0.005;
	k=0;
	while(fabs(g[0])>=0.01 || fabs(g[1])>=0.01 || fabs(g[2])>=0.01 || fabs(g[3])>=0.01)	
	{
		x[0]=c1;
		x[1]=c2;
		x[2]=c3;
		x[3]=c4;

		G[0][0]=c4*2/(a*a);
		G[0][1]=fz;
		G[0][2]=-fy;
		G[0][3]=(2*c1)/(a*a);
		
		G[1][0]=-fz;
		G[1][1]=c4*2/(b*b);
		G[1][2]=fx;
		G[1][3]=(2*(c2-y00))/(b*b);

		G[2][0]=fy;
		G[2][1]=-fx;
		G[2][2]=c4*2/(c*c);
		G[2][3]=(2*(c3-z0))/(c*c);

		G[3][0]=(2*c1)/(a*a);
		G[3][1]=(2*(c2-y00))/(b*b);
		G[3][2]=(2*(c3-z0))/(c*c);
		G[3][3]=0;

		g[0]=c4*((2*c1)/(a*a))-c3*fy+c2*fz-mx;
		g[1]=c4*((2*(c2-y00))/(b*b))-c1*fz+c3*fx-my;
		g[2]=c4*((2*(c3-z0))/(c*c))-c2*fx+c1*fy-mz;
		g[3]=(c1/a)*(c1/a) + ((c2-y00)/b)*((c2-y00)/b) + ((c3-z0)/c)*((c3-z0)/c) - 1;

		x[0]=x[0]-converg*(G[0][0]*g[0]+G[1][0]*g[1]+G[2][0]*g[2]+G[3][0]*g[3]);
		x[1]=x[1]-converg*(G[0][1]*g[0]+G[1][1]*g[1]+G[2][1]*g[2]+G[3][1]*g[3]);
		x[2]=x[2]-converg*(G[0][2]*g[0]+G[1][2]*g[1]+G[2][2]*g[2]+G[3][2]*g[3]);
		x[3]=x[3]-converg*(G[0][3]*g[0]+G[1][3]*g[1]+G[2][3]*g[2]+G[3][3]*g[3]);

		c1=x[0];
		c2=x[1];
		c3=x[2];
		c4=x[3];
		k++;
		if(k>MAX) {
			cl.x=x[0];
			cl.y=x[1];
			cl.z=x[2];
			x4=x[3];
		//	cl_pub.publish(cl);
			return k;
		}
	}
	ROS_DEBUG("k: %d\nfabs(g[0])=%f || fabs(g[1])=%f || fabs(g[2])=%f || fabs(g[3])=%f)",k,fabs(g[0]),fabs(g[1]),fabs(g[2]),fabs(g[3]));
	cl.x=x[0];
	cl.y=x[1];
	cl.z=x[2];
	x4=x[3];
	return k;
}

int Contact::location_deform()
{
	float c1, c2, c3, c4;
	double c5;
	float converg;

	double fx = ftsensor.wrench.force.x;
	double fy = ftsensor.wrench.force.y;
	double fz = ftsensor.wrench.force.z;

	double mx = ftsensor.wrench.torque.x;
	double my = ftsensor.wrench.torque.y;
	double mz = ftsensor.wrench.torque.z;

	/**float x[4] = { 0,0,0,0 };
	int k = 0;
	float g[4] = { 1,1,1,1 };
	float G[4][4];*/

	float x[5] = { 0,0,0,0,0 };
	int k = 0;
	float g[5] = { 1,1,1,1,1 };
	float G[5][5];

	if (finger == 2 && (fz>0.1 || (fy<-0.1 && fy / netforcef<-0.5))) {
		c1 = 0;
		c2 = b + y00;
		c3 = 0;
		c4 = 0;
		c5 = 0;
	}
	else {
		c1 = 0;
		c2 = 0;
		c3 = c + z0;
		c4 = 0;
		c5 = 0;
	}
	converg = 0.005;
	k = 0;
	while (fabs(g[0]) >= 0.01 || fabs(g[1]) >= 0.01 || fabs(g[2]) >= 0.01 || fabs(g[3]) >= 0.01 || fabs(g[4]) >= 0.01)
	{
		x[0] = c1;
		x[1] = c2;
		x[2] = c3;
		x[3] = c4;
		x[4] = c5;

		G[0][0] = c4 * 2 / (a*a);
		G[0][1] = fz;
		G[0][2] = -fy;
		G[0][3] = (2 * c1) / (a*a);
		G[0][4] = 0;

		G[1][0] = -fz;
		G[1][1] = c4 * 2 / (b*b);
		G[1][2] = fx;
		G[1][3] = (2 * (c2 - y00)) / (b*b);
		G[1][4] = 0;

		G[2][0] = fy;
		G[2][1] = -fx;
		G[2][2] = c4 * 2 / (c*c);
		G[2][3] = (2 * (c3 - z0)) / (c*c);
		G[2][4] = 0;

		G[3][0] = (2 * c1) / (a*a);
		G[3][1] = (2 * (c2 - y00)) / (b*b);
		G[3][2] = (2 * (c3 - z0)) / (c*c);
		G[3][3] = 0;
		G[3][4] = 0;

		double v_1; v_1 = (c*c*(2 * c1 + 2 * c2) + 2 * c3*a*b)*(c*c*(2 * c1 + 2 * c2) + 2 * c3*a*b);
		G[4][0] = 4 * c*c*((c2*c*c + c3 * a*b)*fx - c2 * c*c*fy - c3 * a*b*fz) / v_1;
		G[4][1] = 4 * c*c*((c1*c*c + c3 * a*b)*fy - c1 * c*c*fx - c3 * a*b*fz) / v_1;
		G[4][2] = 4 * c*c*a*b*((c1 + c2)*fz - c1 * fx - c2 * fy) / v_1;
		G[4][3] = 0;
		G[4][4] = E;

		g[0] = c4 * ((2 * c1) / (a*a)) - c3 * fy + c2 * fz - mx;
		g[1] = c4 * ((2 * (c2 - y00)) / (b*b)) - c1 * fz + c3 * fx - my;
		g[2] = c4 * ((2 * (c3 - z0)) / (c*c)) - c2 * fx + c1 * fy - mz;
		g[3] = (c1 / a)*(c1 / a) + ((c2 - y00) / b)*((c2 - y00) / b) + ((c3 - z0) / c)*((c3 - z0) / c) - 1;
		g[4] = (2*c1*c*c*fx+2*c2*c*c*fy+2*c3*a*b*fz)/(c*c*(2*c1+2*c2)+2*c3*a*b) - c5 * E;

		x[0] = x[0] - converg * (G[0][0] * g[0] + G[1][0] * g[1] + G[2][0] * g[2] + G[3][0] * g[3] + G[4][0] * g[4]);
		x[1] = x[1] - converg * (G[0][1] * g[0] + G[1][1] * g[1] + G[2][1] * g[2] + G[3][1] * g[3] + G[4][1] * g[4]);
		x[2] = x[2] - converg * (G[0][2] * g[0] + G[1][2] * g[1] + G[2][2] * g[2] + G[3][2] * g[3] + G[4][2] * g[4]);
		x[3] = x[3] - converg * (G[0][3] * g[0] + G[1][3] * g[1] + G[2][3] * g[2] + G[3][3] * g[3] + G[4][3] * g[4]);
		x[4] = x[4] - converg * (G[0][4] * g[0] + G[1][4] * g[1] + G[2][4] * g[2] + G[3][4] * g[3] + G[4][4] * g[4]);

		c1 = x[0];
		c2 = x[1];
		c3 = x[2];
		c4 = x[3];
		c5 = x[4];
		k++;
		if (k>MAX) {
			cl.x = x[0];
			cl.y = x[1];
			cl.z = x[2];
			x4 = x[3];
			deform = x[4];
			//	cl_pub.publish(cl);
			return k;
		}
	}
	ROS_DEBUG("k: %d\nfabs(g[0])=%f || fabs(g[1])=%f || fabs(g[2])=%f || fabs(g[3])=%f)", k, fabs(g[0]), fabs(g[1]), fabs(g[2]), fabs(g[3]));
	cl.x = x[0];
	cl.y = x[1];
	cl.z = x[2];
	x4 = x[3];
	deform = x[4];
	return k;
}

	//	Change frame from ellipsoid center to finger distal frame (used for Shadow type finger)
geometry_msgs::Point Contact::changeFrame(geometry_msgs::Point in_pt)
{
	geometry_msgs::Point new_pt;
	new_pt.x= (in_pt.x)/1000.0;
	new_pt.y= -in_pt.z/1000.0;
	new_pt.z= (in_pt.y+b)/1000.0;
	return new_pt;
}

	//	Change frame from ellipsoid center to finger distal frame (used for Shadow type finger)
geometry_msgs::Vector3 Contact::changeFrame(geometry_msgs::Vector3 in_v)
{
	geometry_msgs::Vector3 new_v;
	new_v.x= in_v.x;
	new_v.y= -in_v.z;
	new_v.z= in_v.y;
	return new_v;
}

void Contact::sensorCallback(const geometry_msgs::WrenchStamped &msg)
{
	double x[4];
	struct timespec tstart={0,0}, tend={0,0};
	double t1,t2;
	int f;	int ret;	int iters;

	ftsensor.wrench.force.x=cfrot*msg.wrench.force.x+sfrot*msg.wrench.force.y;
	ftsensor.wrench.force.y=-sfrot*msg.wrench.force.x+cfrot*msg.wrench.force.y;
	ftsensor.wrench.force.z=msg.wrench.force.z;
	ftsensor.wrench.torque.x=cfrot*msg.wrench.torque.x+sfrot*msg.wrench.torque.y;
	ftsensor.wrench.torque.y=-sfrot*msg.wrench.torque.x+cfrot*msg.wrench.torque.y;
	ftsensor.wrench.torque.z=msg.wrench.torque.z;

	magdir();	
	clock_gettime(CLOCK_MONOTONIC, &tstart);
	if(GD)
	{
		iters=0;
		//iters=location();
		iters=location_deform();
		clock_gettime(CLOCK_MONOTONIC, &tend);
		t1=((double)tend.tv_sec + 1.0e-9*tend.tv_nsec) - ((double)tstart.tv_sec + 1.0e-9*tstart.tv_nsec);
		ROS_INFO("A: %f %f %f %f %f %f => %f %f %f %f", ftsensor.wrench.force.x,ftsensor.wrench.force.y,ftsensor.wrench.force.z,
				ftsensor.wrench.torque.x,ftsensor.wrench.torque.y,ftsensor.wrench.torque.z,cl.x,cl.y,cl.z,x4);
		ROS_INFO("iters:%d | %f ",iters,t1);
		ROS_WARN("Normal Force: %f",abs(Fnormal));
		cout<<green<<"Deformation: "<<deform<<reset<<endl;
		//ROS_WARN("%f",sqrt(ftsensor.wrench.force.x*ftsensor.wrench.force.x+ftsensor.wrench.force.y*ftsensor.wrench.force.y+ftsensor.wrench.force.z*ftsensor.wrench.force.z));
		double ftan=sqrt(pow(Ft.x,2)+pow(Ft.y,2)+pow(Ft.z,2)); //	Compute tangential force
		Fnormtan();
		
		/** // Used to get plots of contact location during surface following
		 *	data = fopen("Normal Force", "a"); 	fprintf(data,"%f\n", abs(Fnormal));	fclose(data); //Create file with normal force values
		 *	data = fopen("Tangential Force", "a"); 	fprintf(data,"%f\n", abs(ftan));	fclose(data); //Create file with tangential force values 
		 *	data =fopen("Contact Location Info", "a"); 	fprintf(data,"%f\t", cl.x);	fprintf(data,"%f\t", cl.y);	fprintf(data,"%f\n", cl.z);	fclose(data); 
		 * */ 
		/** //	Change from ellipsoid frame to distal frame, use when necessary
		 * Ft=changeFrame(Ft);
		 * cl=changeFrame(cl);
		 * normal=changeFrame(normal);
		* */

		ContactState_ant.header.stamp = ros::Time::now();
		ContactState_ant.header.frame_id=frame_id_;
		ContactState_ant.tangential_force = Ft;
		ContactState_ant.contact_position = cl;
		ContactState_ant.contact_normal=normal;
		ContactState_ant.Fnormal=Fnormal;
		ContactState_ant.Ltorque=Ltorque;
		KCL_ContactState_pub_ant.publish(ContactState_ant);

		x[0]=0; x[1]=0; x[2]=0; x[3]=0;
		if(finger==2 && (ftsensor.wrench.torque.z>0.1 || (ftsensor.wrench.torque.y<-0.1 && ftsensor.wrench.torque.y/netforcef<-0.5))){
			p[0]=0;	p[1]=b+y00;	p[2]=0;	p[3]=0;
		}
		else{
			p[0]=0;	p[1]=0;	p[2]=c+z0;	p[3]=0;
		}
		//void (*pf)(double *p, double *jac, int m, int nn, void );
		//pf=&Contact::ForceBal;
		//int ret=dlevmar_der(&(this->ForceBal),&(this->JacForceBal), p, NULL, 4, 6, 1000, opts, info, NULL, NULL, NULL);
		clock_gettime(CLOCK_MONOTONIC, &tstart);
	}
	if(LM)
	{
		ret=0;
		// http://users.ics.forth.gr/~lourakis/levmar/
		//ret=dlevmar_der(ForceBal2,JacForceBal2, p, x, 4, 6, max_iter, opts, info, NULL, NULL, &ftsensor);
		ret=dlevmar_bc_der(ForceBal2,JacForceBal2, p, x, 4, 6,lb,ub,NULL, max_iter, opts, info, NULL, NULL, &ftsensor);	//	Previous one, default
		if(write_to_file) datfile << p[0] << " " << p[1] << " " << p[2] << " " << p[3] << " " << info[0] << " " << info[1] << " " << info[2] << " " << info[3] << " " 
			<< info[4] << " " << info[5] << " " << info[6] << " " << info[7] << " " << info[8] << " " << info[9]  << std::endl;
		clock_gettime(CLOCK_MONOTONIC, &tend);
		t2=((double)tend.tv_sec + 1.0e-9*tend.tv_nsec) - ((double)tstart.tv_sec + 1.0e-9*tstart.tv_nsec);
		ROS_INFO("B: %f %f %f %f %f %f => %f %f %f %f",ftsensor.wrench.force.x,ftsensor.wrench.force.y,ftsensor.wrench.force.z,ftsensor.wrench.torque.x,
			ftsensor.wrench.torque.y,ftsensor.wrench.torque.z,p[0],p[1],p[2],p[3]);
		ROS_INFO("ret: %d | %f",ret,t2);
		ROS_INFO("%f %f %f %f %f %f %f %f %f %f",info[0],info[1],info[2],info[3],info[4],info[5],info[6],info[7],info[8],info[9]);
        printf("\n");
		cl.x=p[0]; cl.y=p[1]; cl.z=p[2]; x4=p[3];
		Fnormtan();
		/** //	Change from ellipsoid frame to distal frame, use when necessary
		 * Ft=changeFrame(Ft);
		 * cl=changeFrame(cl);
		 * normal=changeFrame(normal);
		* */
		if (info[9]<max_iter && info[9]>1)
		{
			ContactState.header.stamp = ros::Time::now();
			ContactState.header.frame_id=frame_id_;
			ContactState.tangential_force = Ft;
			ContactState.contact_position=cl;
			ContactState.contact_normal=normal;
			ContactState.Fnormal=Fnormal;
			ContactState.Ltorque=Ltorque;
		}
		KCL_ContactState_pub.publish(ContactState);
	}
}

//	Can be used to set the finger type
int Contact::setFinger(char *arg)
{
	for(int i=0;i<(int) strlen(arg);i++)
	{
		arg[i]=tolower(arg[i]);
	}
	if(strcmp(arg,"finger1")==0){
		ROS_INFO("Setting Shadow finger tip");
		finger=2;
		a = 8.5; b = 16.0; c = 9.5;
		y00 = -1; z0 = 0.0; frot=-138*pi/180;
	}
	else if(strcmp(arg,"ellipse")==0){
		finger=1;
		a = 9.0; b = 14.0; c = 6.0;
		y00 = 0.0; z0 =0.0;
		frot=-18*pi/180;
	}
	else if(strcmp(arg,"barrett")==0){
		finger=0;
		a = 9.0;	b = 9.0;	c = 9.0;
		y00 = 0.0;	z0 = 11.0;	frot=-18*pi/180;
	}
	else{
		return -1;
	}
	sfrot=sin(frot);
	cfrot=cos(frot);
	ROS_WARN("Using %s finger parameters a %f, b %f , c %f",arg,a,b,c);
	return 0;
}

int main(int argc,char *argv[]) 
{
	ros::init(argc, argv, "ContLocLM");
	Contact cloc;
	ros::NodeHandle n("~");
	cloc.n=n;
	while(ros::ok()){
		ros::spin();
	}
	return 0;
}
