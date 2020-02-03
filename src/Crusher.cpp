#include "Crusher.h"
#include <cmath>

float crush(float v, float g = 1.f, float d = 1.f, float m = 1.f){
	return v * ((1 - m) + g * m * d / ( 1.f + std::fabs( d * v ) ) ) ;
}

void Crusher::process(const ProcessArgs &args) {
	auto& in1 = inputs[IN1_INPUT];
	auto& in2 = inputs[IN2_INPUT];
	auto& out1 = outputs[OUT1_OUTPUT];
	auto& out2 = outputs[OUT2_OUTPUT];
	
	float g = params[GAIN_PARAM].value * 5.f;
	float d = -1.f + 1.f / ( 1.f - 0.99 * params[DRIVE_PARAM].value);
	float m = params[MIX_PARAM].value;

	out1.value = crush(in1.value, g, d, m);

	if( in2.isConnected() ){
		out2.value = crush(in2.value, g, d, m);
	} else {
		out2.value = out1.value;
	}

}

Model* modelCrusher = createModel<Crusher, CrusherWidget>("Crusher");

