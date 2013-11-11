/*
  The contents of this file are dedicated by all of its authors, including

    Michael S. Gashler,
    anonymous contributors,

  to the public domain (http://creativecommons.org/publicdomain/zero/1.0/).

  Note that some moral obligations still exist in the absence of legal ones.
  For example, it would still be dishonest to deliberately misrepresent the
  origin of a work. Although we impose no legal requirements to obtain a
  license, it is beseeming for those who build on the works of others to
  give back useful improvements, or find a way to pay it forward. If
  you would like to cite us, a published paper about Waffles can be found
  at http://jmlr.org/papers/volume12/gashler11a/gashler11a.pdf. If you find
  our code to be useful, the Waffles team would love to hear how you use it.
*/

#include "GNeuralNet.h"
#ifndef MIN_PREDICT
#include "GMath.h"
#endif // MIN_PREDICT
#include "GActivation.h"
#ifndef MIN_PREDICT
#include "GDistribution.h"
#endif // MIN_PREDICT
#include "GError.h"
#include "GRand.h"
#include "GVec.h"
#include "GDom.h"
#ifndef MIN_PREDICT
#include "GHillClimber.h"
#endif  // MIN_PREDICT
#include "GTransform.h"
#ifndef MIN_PREDICT
#include "GSparseMatrix.h"
#include "GDistance.h"
#include "GAssignment.h"
#endif // MIN_PREDICT
#include "GHolders.h"

using std::vector;

namespace GClasses {

GNeuralNetLayer::GNeuralNetLayer(size_t inputs, size_t outputs, GActivationFunction* pActivationFunction)
{
	m_pActivationFunction = pActivationFunction;
	if(!m_pActivationFunction)
		m_pActivationFunction = new GActivationTanH();
	resize(inputs, outputs);
}

GNeuralNetLayer::GNeuralNetLayer(GDomNode* pNode)
: m_weights(pNode->field("weights")), m_bias(3, m_weights.cols())
{
	GDomListIterator it(pNode->field("bias"));
	GVec::deserialize(bias(), it);
	m_pActivationFunction = GActivationFunction::deserialize(pNode->field("act"));
}

GNeuralNetLayer::~GNeuralNetLayer()
{
	delete(m_pActivationFunction);
}

GDomNode* GNeuralNetLayer::serialize(GDom* pDoc)
{
	GDomNode* pNode = pDoc->newObj();
	pNode->addField(pDoc, "weights", m_weights.serialize(pDoc));
	pNode->addField(pDoc, "bias", GVec::serialize(pDoc, bias(), m_weights.cols()));
	pNode->addField(pDoc, "act", m_pActivationFunction->serialize(pDoc));
	return pNode;
}

void GNeuralNetLayer::resize(size_t inputs, size_t outputs)
{
	m_weights.resize(inputs, outputs);
	m_bias.resize(3, outputs);
}

void GNeuralNetLayer::resizePreserve(size_t inputCount, size_t outputCount, GRand& rand)
{
	size_t oldInputs = inputs();
	size_t oldOutputs = outputs();
	size_t fewerInputs = std::min(oldInputs, inputCount);
	size_t fewerOutputs = std::min(oldOutputs, outputCount);
	GMatrix old;
	old.copy(&m_weights);
	m_weights.resize(inputCount, outputCount);
	for(size_t i = 0; i < fewerInputs; i++)
	{
		double* pRow = m_weights[i];
		GVec::copy(pRow, old[i], fewerOutputs);
		pRow += fewerOutputs;
		for(size_t j = fewerOutputs; j < outputCount; j++)
			*(pRow++) = 0.01 * rand.normal();
	}
	for(size_t i = fewerInputs; i < inputCount; i++)
	{
		double* pRow = m_weights[i];
		for(size_t j = 0; j < outputCount; j++)
			*(pRow++) = 0.01 * rand.normal();
	}
	GVec::copy(old[0], bias(), fewerOutputs);
	m_bias.resize(3, outputCount);
	double* pB = bias();
	GVec::copy(pB, old[0], fewerOutputs);
	pB += fewerOutputs;
	for(size_t j = fewerOutputs; j < outputCount; j++)
		*(pB++) = 0.01 * rand.normal();
}

void GNeuralNetLayer::resetWeights(GRand* pRand)
{
	size_t outputs = m_weights.cols();
	size_t inputs = m_weights.rows();
	double* pB = bias();
	for(size_t i = 0; i < outputs; i++)
	{
		*pB = pRand->normal() * 0.1;
		for(size_t j = 0; j < inputs; j++)
			m_weights[j][i] = pRand->normal() * 0.1;
		pB++;
	}
}

void GNeuralNetLayer::feedForward(const double* pIn)
{
	// Compute net = pIn * m_weights + bias
	size_t outputs = m_weights.cols();
	double* pNet = net();
	GVec::setAll(pNet, 0.0, outputs);
	for(size_t i = 0; i < m_weights.rows(); i++)
		GVec::addScaled(pNet, *(pIn++), m_weights.row(i), outputs);
	GVec::add(pNet, bias(), outputs);

	// Apply the activation function
	double* pAct = activation();
	for(size_t i = 0; i < outputs; i++)
		*(pAct++) = m_pActivationFunction->squash(*(pNet++));
}

void GNeuralNetLayer::feedForwardWithInputBias(const double* pIn)
{
	size_t outputs = m_weights.cols();
	double* pNet = net();
	GVec::setAll(pNet, *(pIn++), outputs);
	for(size_t i = 0; i < m_weights.rows(); i++)
		GVec::addScaled(pNet, *(pIn++), m_weights.row(i), outputs);
	GVec::add(pNet, bias(), outputs);

	// Apply the activation function
	double* pAct = activation();
	for(size_t i = 0; i < outputs; i++)
		*(pAct++) = m_pActivationFunction->squash(*(pNet++));
}

void GNeuralNetLayer::feedForwardToOneOutput(const double* pIn, size_t output, bool inputBias)
{
	// Compute net = pIn * m_weights + bias
	GAssert(output < m_weights.cols());
	double* pNet = net() + output;
	*pNet = inputBias ? *(pIn++) : 0.0;
	for(size_t i = 0; i < m_weights.rows(); i++)
		*pNet += *(pIn++) * m_weights[i][output];
	*pNet += bias()[output];

	// Apply the activation function
	double* pAct = activation() + output;
	*pAct = m_pActivationFunction->squash(*pNet);
}

void GNeuralNetLayer::outputGradient(const double* pIn)
{
	// Compute net = pIn * m_weights + bias
	size_t outputs = m_weights.cols();
	double* pNet = net();
	GVec::setAll(pNet, 0.0, outputs);
	for(size_t i = 0; i < m_weights.rows(); i++)
		GVec::addScaled(pNet, *(pIn++), m_weights.row(i), outputs);
	GVec::add(pNet, bias(), outputs);

	// Apply the activation function
	double* pAct = activation();
	for(size_t i = 0; i < outputs; i++)
		*(pNet++) *= m_pActivationFunction->derivativeOfNet(0.0, *(pAct++));
}

void GNeuralNetLayer::setActivationFunction(GActivationFunction* pAF)
{
	delete(m_pActivationFunction);
	m_pActivationFunction = pAF;
}

void GNeuralNetLayer::transformWeights(GMatrix& transform, const double* pOffset)
{
	if(transform.rows() != inputs())
		throw Ex("Transformation matrix not suitable size for this layer");
	if(transform.rows() != transform.cols())
		throw Ex("Expected a square transformation matrix.");
	size_t outputs = m_weights.cols();
	GMatrix* pNewWeights = GMatrix::multiply(transform, m_weights, true, false);
	Holder<GMatrix> hNewWeights(pNewWeights);
	m_weights.copyBlock(*pNewWeights, 0, 0, pNewWeights->rows(), outputs, 0, 0, false);
	double* pNet = net();
	GVec::setAll(pNet, 0.0, outputs);
	for(size_t i = 0; i < m_weights.rows(); i++)
		GVec::addScaled(pNet, *(pOffset++), m_weights.row(i), outputs);
	GVec::add(bias(), pNet, outputs);
}

void GNeuralNetLayer::perturbWeights(GRand& rand, double deviation)
{
	size_t outs = outputs();
	for(size_t j = 0; j < m_weights.rows(); j++)
		GVec::perturb(m_weights[j], deviation, outs, rand);
	GVec::perturb(bias(), deviation, outs, rand);
}

void GNeuralNetLayer::setToWeaklyApproximateIdentity()
{
	double d = 1.0 / m_pActivationFunction->derivative(0.0);
	double b = -m_pActivationFunction->center() * d;
	m_weights.setAll(0.0);
	size_t n = std::min(inputs(), outputs());
	for(size_t i = 0; i < n; i++)
	{
		m_weights[i][i] = d;
		bias()[i] = b;
	}
}

void GNeuralNetLayer::clipWeights(double max)
{
	size_t outputs = m_weights.cols();
	for(size_t j = 0; j < m_weights.rows(); j++)
	{
		GVec::floorValues(m_weights[j], -max, outputs);
		GVec::capValues(m_weights[j], max, outputs);
	}
}

// ----------------------------------------------------------------------

void GBackPropLayer::resize(size_t inputs, size_t outputs)
{
	m_delta.resize(inputs, outputs);
	m_delta.setAll(0.0);
	m_blame.resize(3, outputs);
	GVec::setAll(slack(), 0.0, outputs);
	GVec::setAll(biasDelta(), 0.0, outputs);
}

// ----------------------------------------------------------------------

GBackProp::GBackProp(GNeuralNet* pNN)
: m_pNN(pNN)
{
	if(!m_pNN->hasTrainingBegun())
		throw Ex("The specified neural network is not yet ready for training");

	// Initialize structures to mirror the neural network
	m_layers.resize(m_pNN->m_layers.size());
	for(size_t i = 0; i < m_layers.size(); i++)
	{
		GBackPropLayer& layer = m_layers[i];
		layer.resize(pNN->m_layers[i]->inputs(), pNN->m_layers[i]->outputs());
	}
}

GBackProp::~GBackProp()
{
}

void GBackProp::computeBlame(const double* pTarget, size_t layer, TargetFunction eTargetFunction)
{
	// Compute error on output layer
	layer = std::min(layer, m_layers.size() - 1);
	GBackPropLayer& bpOutputLayer = m_layers[layer];
	GNeuralNetLayer& nnOutputLayer = *m_pNN->m_layers[layer];
	size_t outputs = nnOutputLayer.outputs();
	double* pNet = nnOutputLayer.net();
	double* pAct = nnOutputLayer.activation();
	double* pSlack = bpOutputLayer.slack();
	double* pBlame = bpOutputLayer.blame();
	switch(eTargetFunction)
	{
		case squared_error:
			{
				for(size_t i = 0; i < outputs; i++)
				{
					if(*pTarget == UNKNOWN_REAL_VALUE)
						*pBlame = 0.0;
					else
					{
						if(*pTarget > *pAct + *pSlack)
							*pBlame = (*pTarget - *pAct - *pSlack) * nnOutputLayer.m_pActivationFunction->derivativeOfNet(*pNet, *pAct);
						else if(*pTarget < *pAct - *pSlack)
							*pBlame = (*pTarget - *pAct + *pSlack) * nnOutputLayer.m_pActivationFunction->derivativeOfNet(*pNet, *pAct);
						else
							*pBlame = 0.0;
					}
					pTarget++;
					pSlack++;
					pAct++;
					pNet++;
					pBlame++;
				}
			}
			break;

		case cross_entropy:
			{
				for(size_t i = 0; i < outputs; i++)
				{
					if(*pTarget == UNKNOWN_REAL_VALUE)
						*pBlame = 0.0;
					else
					{
						if(*pTarget > *pAct + *pSlack)
							*pBlame = (*pTarget - *pAct - *pSlack);
						else if(*pTarget < *pAct - *pSlack)
							*pBlame = (*pTarget - *pAct + *pSlack);
						else
							*pBlame = 0.0;
					}
					pTarget++;
					pSlack++;
					pAct++;
					pNet++;
					pBlame++;
				}
			}
			break;

		case sign:
			{
				for(size_t i = 0; i < outputs; i++)
				{
					if(*pTarget == UNKNOWN_REAL_VALUE)
						*pBlame = 0.0;
					else
					{
						if(*pTarget > *pAct + *pSlack)
							*pBlame = nnOutputLayer.m_pActivationFunction->derivativeOfNet(*pNet, *pAct);
						else if(*pTarget < *pAct - *pSlack)
							*pBlame = -nnOutputLayer.m_pActivationFunction->derivativeOfNet(*pNet, *pAct);
						else
							*pBlame = 0.0;
					}
					pTarget++;
					pSlack++;
					pAct++;
					pNet++;
					pBlame++;
				}
			}
			break;

		default:
			throw Ex("Unrecognized target function for back-propagation");
			break;
	}
}

void GBackProp::computeBlameSingleOutput(double target, size_t output, size_t layer, TargetFunction eTargetFunction)
{
	layer = std::min(layer, m_layers.size() - 1);
	GBackPropLayer& bpOutputLayer = m_layers[layer];
	double slack = bpOutputLayer.slack()[output];
	double* pBlame = bpOutputLayer.blame() + output;
	GNeuralNetLayer& nnOutputLayer = *
	m_pNN->m_layers[layer];
	double net = nnOutputLayer.net()[output];
	double act = nnOutputLayer.activation()[output];
	switch(eTargetFunction)
	{
		case squared_error:
			if(target > act + slack)
				*pBlame = (target - act - slack) * nnOutputLayer.m_pActivationFunction->derivativeOfNet(net, act);
			else if(target < act - slack)
				*pBlame = (target - act + slack) * nnOutputLayer.m_pActivationFunction->derivativeOfNet(net, act);
			else
				*pBlame = 0.0;
			break;

		case cross_entropy:
			if(target > act + slack)
				*pBlame = (target - act - slack);
			else if(target < act - slack)
				*pBlame = (target - act + slack);
			else
				*pBlame = 0.0;
			break;

		case sign:
			if(target > act + slack)
				*pBlame = nnOutputLayer.m_pActivationFunction->derivativeOfNet(net, act);
			else if(target < act - slack)
				*pBlame = -nnOutputLayer.m_pActivationFunction->derivativeOfNet(net, act);
			else
				*pBlame = 0.0;
			break;

		default:
			throw Ex("Unrecognized target function for back-propagation");
			break;
	}
}

void GBackProp::backPropLayer(GNeuralNetLayer* pNNDownStreamLayer, GNeuralNetLayer* pNNUpStreamLayer, GBackPropLayer* pBPDownStreamLayer, GBackPropLayer* pBPUpStreamLayer)
{
	GMatrix& w = pNNDownStreamLayer->m_weights;
	size_t outputs = w.cols();
	double* pIn = pBPDownStreamLayer->blame();
	double* pOut = pBPUpStreamLayer->blame();
	double* pNet = pNNUpStreamLayer->net();
	double* pAct = pNNUpStreamLayer->activation();
	for(size_t i = 0; i < w.rows(); i++)
		*(pOut++) = GVec::dotProduct(pIn, w[i], outputs) * pNNUpStreamLayer->m_pActivationFunction->derivativeOfNet(*(pNet++), *(pAct++));
}

void GBackProp::backpropagate(size_t startLayer)
{
	size_t i = std::min(startLayer, m_layers.size() - 1);
	GNeuralNetLayer* pNNDownStreamLayer = m_pNN->m_layers[i];
	GBackPropLayer* pBPDownStreamLayer = &m_layers[i];
	for(i--; i < m_layers.size(); i--)
	{
		GNeuralNetLayer* pNNUpStreamLayer = m_pNN->m_layers[i];
		GBackPropLayer* pBPUpStreamLayer = &m_layers[i];
		backPropLayer(pNNDownStreamLayer, pNNUpStreamLayer, pBPDownStreamLayer, pBPUpStreamLayer);
		pNNDownStreamLayer = pNNUpStreamLayer;
		pBPDownStreamLayer = pBPUpStreamLayer;
	}
}

void GBackProp::backPropFromSingleNode(size_t outputNode, GNeuralNetLayer* pNNDownStreamLayer, GNeuralNetLayer* pNNUpStreamLayer, GBackPropLayer* pBPDownStreamLayer, GBackPropLayer* pBPUpStreamLayer)
{
	GMatrix& w = pNNDownStreamLayer->m_weights;
	GAssert(outputNode < w.cols());
	double in = pBPDownStreamLayer->blame()[outputNode];
	double* pOut = pBPUpStreamLayer->blame();
	double* pNet = pNNUpStreamLayer->net();
	double* pAct = pNNUpStreamLayer->activation();
	for(size_t i = 0; i < w.rows(); i++)
		*(pOut++) = in * w[i][outputNode] * pNNUpStreamLayer->m_pActivationFunction->derivativeOfNet(*(pNet++), *(pAct++));
}

void GBackProp::backpropagateSingleOutput(size_t outputNode, size_t startLayer)
{
	size_t i = std::min(startLayer, m_layers.size() - 1);
	GNeuralNetLayer* pNNDownStreamLayer = m_pNN->m_layers[i];
	GBackPropLayer* pBPDownStreamLayer = &m_layers[i];
	if(--i < m_layers.size())
	{
		GNeuralNetLayer* pNNUpStreamLayer = m_pNN->m_layers[i];
		GBackPropLayer* pBPUpStreamLayer = &m_layers[i];
		backPropFromSingleNode(
			outputNode, pNNDownStreamLayer, pNNUpStreamLayer, pBPDownStreamLayer, pBPUpStreamLayer);
		pNNDownStreamLayer = pNNUpStreamLayer;
		pBPDownStreamLayer = pBPUpStreamLayer;
		for(i--; i < m_layers.size(); i--)
		{
			pNNUpStreamLayer = m_pNN->m_layers[i];
			pBPUpStreamLayer = &m_layers[i];
			backPropLayer(pNNDownStreamLayer, pNNUpStreamLayer, pBPDownStreamLayer, pBPUpStreamLayer);
			pNNDownStreamLayer = pNNUpStreamLayer;
			pBPDownStreamLayer = pBPUpStreamLayer;
		}
	}
}

void GBackProp::adjustWeights(GNeuralNetLayer* pNNDownStreamLayer, const double* pUpStreamActivation, GBackPropLayer* pBPDownStreamLayer, double learningRate, double momentum)
{
	// Adjust the weights
	double* pBlame = pBPDownStreamLayer->blame();
	GMatrix& w = pNNDownStreamLayer->m_weights;
	GMatrix& delta = pBPDownStreamLayer->m_delta;
	size_t outputs = w.cols();
	for(size_t up = 0; up < w.rows(); up++)
	{
		double* pB = pBlame;
		double* pD = delta[up];
		double* pW = w[up];
		double act = *(pUpStreamActivation++);
		for(size_t down = 0; down < outputs; down++)
		{
			*pD *= momentum;
			*pD += (*(pB++) * learningRate * act);
			GAssert(*pD * *pD < 2.0 * pNNDownStreamLayer->m_pActivationFunction->halfRange() / (1.0 - momentum)); // Either the blame, rate, or activation is unreasonable
			*(pW++) += *(pD++);
		}
	}

	// Adjust the bias
	double* pB = pBlame;
	double* pD = pBPDownStreamLayer->biasDelta();
	double* pW = pNNDownStreamLayer->bias();
	for(size_t down = 0; down < outputs; down++)
	{
		*pD *= momentum;
		*pD += (*(pB++) * learningRate);
		*(pW++) += *(pD++);
	}
}

void GBackProp::adjustWeightsSingleNeuron(size_t outputNode, GNeuralNetLayer* pNNDownStreamLayer, const double* pUpStreamActivation, GBackPropLayer* pBPDownStreamLayer, double learningRate, double momentum)
{
	// Adjust the weights
	double blame = pBPDownStreamLayer->blame()[outputNode];
	GMatrix& w = pNNDownStreamLayer->m_weights;
	GMatrix& delta = pBPDownStreamLayer->m_delta;
	for(size_t up = 0; up < w.rows(); up++)
	{
		double* pD = &delta[up][outputNode];
		double* pW = &w[up][outputNode];
		double act = *(pUpStreamActivation++);
		*pD *= momentum;
		*pD += (blame * learningRate * act);
		*pW = std::max(-1e12, std::min(1e12, *pW + *pD));
	}

	// Adjust the bias
	double* pD = &pBPDownStreamLayer->biasDelta()[outputNode];
	double* pW = &pNNDownStreamLayer->bias()[outputNode];
	*pD *= momentum;
	*pD += (blame * learningRate);
	*pW = std::max(-1e12, std::min(1e12, *pW + *pD));
}

class LagrangeVals
{
public:
	double learningRate;
	double momentum;
	double err;
	double lambda_sprime;
	double lambda_sdoubleprime;
	double lambda_out;
};

void GBackProp::adjustWeightsLagrange(GNeuralNetLayer* pNNDownStreamLayer, const double* pUpStreamActivation, GBackPropLayer* pBPDownStreamLayer, LagrangeVals& lv)
{
	// Adjust the weights
	double* pBlame = pBPDownStreamLayer->blame();
	GMatrix& w = pNNDownStreamLayer->m_weights;
	GMatrix& delta = pBPDownStreamLayer->m_delta;
	size_t outputs = w.cols();
	for(size_t up = 0; up < w.rows(); up++)
	{
		double* pB = pBlame;
		double* pD = delta[up];
		double* pW = w[up];
		double act = *(pUpStreamActivation++);
		for(size_t down = 0; down < outputs; down++)
		{
			*pD *= lv.momentum;
			*pD += lv.learningRate * (lv.err * *pB * act - (*pW - lv.lambda_sprime * act) * (1.0 - lv.lambda_sdoubleprime * *pB * act * act));
			lv.lambda_out += lv.learningRate * (*pW - lv.lambda_sprime * act) * lv.lambda_sprime * act;
			*(pW++) += *(pD++);
			pB++;
		}
	}

	// Adjust the bias
	double* pB = pBlame;
	double* pD = pBPDownStreamLayer->biasDelta();
	double* pW = pNNDownStreamLayer->bias();
	for(size_t down = 0; down < outputs; down++)
	{
		*pD *= lv.momentum;
		*pD += lv.learningRate * (lv.err * *pB - (*pW - lv.lambda_sprime) * (1.0 - lv.lambda_sdoubleprime * *pB));
		lv.lambda_out += lv.learningRate * (*pW - lv.lambda_sprime) * lv.lambda_sprime;
		*(pW++) += *(pD++);
		pB++;
	}
}

void GBackProp::descendGradient(const double* pFeatures, double learningRate, double momentum)
{
	for(size_t i = m_layers.size() - 1; i > 0; i--)
		adjustWeights(m_pNN->m_layers[i], m_pNN->m_layers[i - 1]->activation(), &m_layers[i], learningRate, momentum);
	adjustWeights(m_pNN->m_layers[0], pFeatures + (m_pNN->useInputBias() ? 1 : 0), &m_layers[0], learningRate, momentum);
}

void GBackProp::descendGradientOneLayer(size_t layer, const double* pFeatures, double learningRate, double momentum)
{
	if(layer > 0)
		adjustWeights(m_pNN->m_layers[layer], m_pNN->m_layers[layer - 1]->activation(), &m_layers[layer], learningRate, momentum);
	else
		adjustWeights(m_pNN->m_layers[layer], pFeatures + (m_pNN->useInputBias() ? 1 : 0), &m_layers[layer], learningRate, momentum);
}

void GBackProp::descendGradientSingleOutput(size_t outputNeuron, const double* pFeatures, double learningRate, double momentum)
{
	size_t i = m_layers.size() - 1;
	if(i == 0)
		adjustWeightsSingleNeuron(outputNeuron, m_pNN->m_layers[0], pFeatures + (m_pNN->useInputBias() ? 1 : 0), &m_layers[0], learningRate, momentum);
	else
	{
		adjustWeightsSingleNeuron(outputNeuron, m_pNN->m_layers[i], m_pNN->m_layers[i - 1]->activation(), &m_layers[i], learningRate, momentum);
		for(i--; i > 0; i--)
			adjustWeights(m_pNN->m_layers[i], m_pNN->m_layers[i - 1]->activation(), &m_layers[i], learningRate, momentum);
		adjustWeights(
			m_pNN->m_layers[0], pFeatures + (m_pNN->useInputBias() ? 1 : 0), &m_layers[0], learningRate, momentum);
	}
}

void GBackProp::gradientOfInputs(double* pOutGradient)
{
	GMatrix& w = m_pNN->m_layers[0]->m_weights;
	size_t outputs = w.cols();
	double* pBlame = m_layers[0].blame();
	if(m_pNN->useInputBias())
		*(pOutGradient++) = -GVec::sumElements(pBlame, outputs);
	for(size_t i = 0; i < w.rows(); i++)
		*(pOutGradient++) = -GVec::dotProduct(w[i], pBlame, outputs);
}

void GBackProp::gradientOfInputsSingleOutput(size_t outputNeuron, double* pOutGradient)
{
	if(m_layers.size() != 1)
	{
		gradientOfInputs(pOutGradient);
		return;
	}
	GMatrix& w = m_pNN->m_layers[0]->m_weights;
	GAssert(outputNeuron < w.cols());

	double* pBlame = m_layers[0].blame();
	if(m_pNN->useInputBias())
		*(pOutGradient++) = -pBlame[outputNeuron];
	for(size_t i = 0; i < w.rows(); i++)
		*(pOutGradient++) = -pBlame[outputNeuron] * w[i][outputNeuron];
}

// ----------------------------------------------------------------------

GNeuralNet::GNeuralNet()
: GIncrementalLearner(),
m_pBackProp(NULL),
m_learningRate(0.1),
m_momentum(0.0),
m_validationPortion(0.35),
m_minImprovement(0.002),
m_epochsPerValidationCheck(100),
m_backPropTargetFunction(GBackProp::squared_error),
m_useInputBias(false)
{
}

GNeuralNet::GNeuralNet(GDomNode* pNode, GLearnerLoader& ll)
: GIncrementalLearner(pNode, ll),
m_pBackProp(NULL),
m_validationPortion(0.35),
m_minImprovement(0.002),
m_epochsPerValidationCheck(100)
{
	m_learningRate = pNode->field("learningRate")->asDouble();
	m_momentum = pNode->field("momentum")->asDouble();
	m_backPropTargetFunction = (GBackProp::TargetFunction)pNode->field("target")->asInt();
	m_useInputBias = pNode->field("ib")->asBool();

	// Create the layers
	GDomListIterator it1(pNode->field("layers"));
	while(it1.remaining() > 0)
	{
		m_layers.push_back(new GNeuralNetLayer(it1.current()));
		it1.advance();
	}

	// Enable training
	m_pBackProp = new GBackProp(this);
}

GNeuralNet::~GNeuralNet()
{
	clear();
}

void GNeuralNet::clear()
{
	releaseTrainingJunk();
	for(size_t i = 0; i < m_layers.size(); i++)
		delete(m_layers[i]);
	m_layers.clear();
}

void GNeuralNet::setTopology(size_t h1, size_t h2, size_t h3, size_t h4, size_t h5, size_t h6)
{
	vector<size_t> topo;
	if(h1 > 0) topo.push_back(h1);
	if(h2 > 0) topo.push_back(h2);
	if(h3 > 0) topo.push_back(h3);
	if(h4 > 0) topo.push_back(h4);
	if(h5 > 0) topo.push_back(h5);
	if(h6 > 0) topo.push_back(h6);
	setTopology(topo);
}

#ifndef MIN_PREDICT
// virtual
GDomNode* GNeuralNet::serialize(GDom* pDoc) const
{
	return serializeInner(pDoc, "GNeuralNet");
}

GDomNode* GNeuralNet::serializeInner(GDom* pDoc, const char* szClassName) const
{
	if(!hasTrainingBegun())
		throw Ex("The network has not been trained");
	GDomNode* pNode = baseDomNode(pDoc, szClassName);

	// Add the layers
	GDomNode* pLayerList = pNode->addField(pDoc, "layers", pDoc->newList());
	for(size_t i = 0; i < m_layers.size(); i++)
		pLayerList->addItem(pDoc, m_layers[i]->serialize(pDoc));

	// Add other settings
	pNode->addField(pDoc, "learningRate", pDoc->newDouble(m_learningRate));
	pNode->addField(pDoc, "momentum", pDoc->newDouble(m_momentum));
	pNode->addField(pDoc, "target", pDoc->newInt(m_backPropTargetFunction));
	pNode->addField(pDoc, "ib", pDoc->newBool(m_useInputBias));

	return pNode;
}
#endif // MIN_PREDICT

// virtual
bool GNeuralNet::supportedFeatureRange(double* pOutMin, double* pOutMax)
{
	*pOutMin = -1.0;
	*pOutMax = 1.0;
	return false;
}

// virtual
bool GNeuralNet::supportedLabelRange(double* pOutMin, double* pOutMax)
{
	if(m_layers.size() > 0)
	{
		GActivationFunction* pAct = m_layers[m_layers.size() - 1]->m_pActivationFunction;
		double hr = pAct->halfRange();
		if(hr >= 1e50)
			return true;
		double c = pAct->center();
		*pOutMin = c - hr;
		*pOutMax = c + hr;
	}
	else
	{
		// Assume the tanh function is the default
		*pOutMin = -1.0;
		*pOutMax = 1.0;
	}
	return false;
}

void GNeuralNet::releaseTrainingJunk()
{
	delete(m_pBackProp);
	m_pBackProp = NULL;
}

void GNeuralNet::addNodes(size_t layer, size_t nodeCount)
{
	if(layer >= m_layers.size())
		throw Ex("layer index out of range");

	// Add columns to the upstream layer
	GNeuralNetLayer* pUpStream = m_layers[layer];
	size_t in = pUpStream->inputs();
	size_t out = pUpStream->outputs();
	pUpStream->m_weights.newColumns(nodeCount);
	pUpStream->m_bias.newColumns(nodeCount);
	for(size_t i = 0; i < in; i++)
	{
		for(size_t j = 0; j < nodeCount; j++)
			pUpStream->m_weights[i][out + j] = 0.01 * m_rand.normal();
	}
	for(size_t j = 0; j < nodeCount; j++)
		pUpStream->bias()[out + j] = 0.01 * m_rand.normal();

	// Add rows to the downstream layer
	if(layer + 1 < m_layers.size())
	{
		for(size_t j = 0; j < nodeCount; j++)
		{
			double* pRow = m_layers[layer + 1]->m_weights.newRow();
			for(size_t i = 0; i < out; i++)
				*(pRow++) = 0.01 * m_rand.normal();
		}
	}
}

void GNeuralNet::dropNode(size_t layer, size_t node)
{
	if(layer >= m_layers.size())
		throw Ex("layer index out of range");

	GNeuralNetLayer* pUpStream = m_layers[layer];
	pUpStream->m_weights.deleteColumn(node);
	pUpStream->m_bias.deleteColumn(node);
	if(layer + 1 < m_layers.size())
		m_layers[layer + 1]->m_weights.deleteRow(node);
}

size_t GNeuralNet::countWeights() const
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	size_t wc = 0;
	for(size_t i = 0; i < m_layers.size(); i++)
		wc += (m_layers[i]->inputs() + 1) * m_layers[i]->outputs();
	return wc;
}

void GNeuralNet::weights(double* pOutWeights) const
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	for(size_t i = 0; i < m_layers.size(); i++)
	{
		GNeuralNetLayer& lay = *m_layers[i];
		GVec::copy(pOutWeights, lay.bias(), lay.outputs());
		pOutWeights += lay.outputs();
		lay.m_weights.toVector(pOutWeights);
		pOutWeights += (lay.inputs() * lay.outputs());
	}
}

void GNeuralNet::setWeights(const double* pWeights)
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	for(size_t i = 0; i < m_layers.size(); i++)
	{
		GNeuralNetLayer& lay = *m_layers[i];
		GVec::copy(lay.bias(), pWeights, lay.outputs());
		pWeights += lay.outputs();
		lay.m_weights.fromVector(pWeights, lay.inputs());
		pWeights += (lay.inputs() * lay.outputs());
	}
}

void GNeuralNet::copyWeights(GNeuralNet* pOther)
{
	if(!hasTrainingBegun() || !pOther->hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called on both networks before this method");
	for(size_t i = 0; i < m_layers.size(); i++)
	{
		GNeuralNetLayer& src = *pOther->m_layers[i];
		GNeuralNetLayer& dest = *m_layers[i];
		dest.m_weights.copyBlock(src.m_weights, 0, 0, INVALID_INDEX, INVALID_INDEX, 0, 0, false);
		GVec::copy(dest.bias(), src.bias(), src.outputs());
	}
}

void GNeuralNet::copyStructure(GNeuralNet* pOther)
{
	if(!pOther->hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	clear();
	for(size_t i = 0; i < pOther->m_layers.size(); i++)
		m_layers.push_back(new GNeuralNetLayer(pOther->m_layers[i]->inputs(), pOther->m_layers[i]->outputs(), pOther->m_layers[i]->m_pActivationFunction->clone()));
	m_learningRate = pOther->m_learningRate;
	m_momentum = pOther->m_momentum;
	m_validationPortion = pOther->m_validationPortion;
	m_minImprovement = pOther->m_minImprovement;
	m_epochsPerValidationCheck = pOther->m_epochsPerValidationCheck;
	m_backPropTargetFunction = pOther->m_backPropTargetFunction;
	m_useInputBias = pOther->m_useInputBias;
	if(pOther->m_pBackProp)
		m_pBackProp = new GBackProp(this);
}

void GNeuralNet::perturbAllWeights(double deviation)
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	for(size_t i = 0; i < m_layers.size(); i++)
		m_layers[i]->perturbWeights(m_rand, deviation);
}

void GNeuralNet::clipWeights(double max)
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	for(size_t i = 0; i < m_layers.size(); i++)
		m_layers[i]->clipWeights(max);
}

void GNeuralNet::invertNode(size_t layer, size_t node)
{
	GNeuralNetLayer& layerUpStream = *m_layers[layer];
	GMatrix& w = layerUpStream.m_weights;
	for(size_t i = 0; i < w.rows(); i++)
		w[i][node] = -w[i][node];
	layerUpStream.bias()[node] = -layerUpStream.bias()[node];
	if(layer + 1 < m_layers.size())
	{
		GNeuralNetLayer& layerDownStream = *m_layers[layer + 1];
		size_t downOuts = layerDownStream.outputs();
		double* pW = layerDownStream.m_weights[node];
		double* pB = layerDownStream.bias();
		for(size_t i = 0; i < downOuts; i++)
		{
			pB[i] += 2 * layerUpStream.m_pActivationFunction->center() * pW[i];
			pW[i] = -pW[i];
		}
	}
}

void GNeuralNet::swapNodes(size_t layer, size_t a, size_t b)
{
	GNeuralNetLayer& layerUpStream = *m_layers[layer];
	layerUpStream.m_weights.swapColumns(a, b);
	std::swap(layerUpStream.bias()[a], layerUpStream.bias()[b]);
	if(layer + 1 < m_layers.size())
	{
		GNeuralNetLayer& layerDownStream = *m_layers[layer + 1];
		layerDownStream.m_weights.swapRows(a, b);
	}
}

void GNeuralNet::insertLayer(size_t position, size_t nodeCount)
{
	if(!hasTrainingBegun())
		throw Ex("insertLayer is only usable after training has begun");

	// Determine the number of inputs into this layer
	size_t inputs;
	if(position < m_layers.size())
		inputs = m_layers[0]->inputs();
	else
	{
		inputs = m_layers[m_layers.size() - 1]->outputs();
		if(nodeCount != inputs)
			throw Ex("This operation is not allowed to change the number of nodes in the output layer");
	}

	// Make the new layer
	GNeuralNetLayer* pNewLayer = new GNeuralNetLayer(inputs, nodeCount);
	pNewLayer->setToWeaklyApproximateIdentity();
	pNewLayer->perturbWeights(m_rand, 0.01);

	// Make sure the next layer is ready for it
	if(position < m_layers.size())
	{
		GNeuralNetLayer& downStreamLayer = *m_layers[position];
		downStreamLayer.resizePreserve(nodeCount, downStreamLayer.outputs(), m_rand);
	}

	// Insert the layer
	m_layers.insert(m_layers.begin() + position, pNewLayer);

	if(m_pBackProp)
	{
		delete(m_pBackProp);
		m_pBackProp = new GBackProp(this);
	}

}

#ifndef MIN_PREDICT
void GNeuralNet::align(const GNeuralNet& that)
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	if(layerCount() != that.layerCount())
		throw Ex("mismatching number of layers");
	for(size_t i = 0; i + 1 < m_layers.size(); i++)
	{
		// Copy weights into matrices
		GNeuralNetLayer& layerThisCur = *m_layers[i];
		const GNeuralNetLayer& layerThatCur = *that.m_layers[i];
		if(layerThisCur.outputs() != layerThatCur.outputs())
			throw Ex("mismatching layer size");

		GMatrix costs(layerThisCur.outputs(), layerThatCur.outputs());
		for(size_t k = 0; k < layerThisCur.outputs(); k++)
		{
			for(size_t j = 0; j < layerThatCur.outputs(); j++)
			{
				double d = layerThisCur.bias()[k] - layerThatCur.bias()[j];
				double pos = d * d;
				d = layerThisCur.bias()[k] + layerThatCur.bias()[j];
				double neg = d * d;
				GMatrix& wThis = layerThisCur.m_weights;
				const GMatrix& wThat = layerThatCur.m_weights;
				for(size_t l = 0; l < layerThisCur.inputs(); l++)
				{
					d = wThis[l][k] - wThat[l][j];
					pos += (d * d);
					d = wThis[l][k] + wThat[l][j];
					neg += (d * d);
				}
				costs[j][k] = std::min(pos, neg);
			}
		}
		GSimpleAssignment indexes = linearAssignment(costs);

		// Align this layer with that layer
		for(size_t j = 0; j < layerThisCur.outputs(); j++)
		{
			size_t k = indexes(j);
			if(k != j)
			{
				// Fix up the indexes
				size_t m = j + 1;
				for( ; m < layerThisCur.outputs(); m++)
				{
					if((size_t)indexes(m) == j)
						break;
				}
				GAssert(m < layerThisCur.outputs());
				indexes.assign(m, k);

				// Swap nodes j and k
				swapNodes(i, j, k);
			}

			// Test whether not j needs to be inverted by computing the dot product of the two weight vectors
			double dp = 0.0;
			size_t inputs = layerThisCur.inputs();
			for(size_t k = 0; k < inputs; k++)
				dp += layerThisCur.m_weights[k][j] * layerThatCur.m_weights[k][j];
			dp += layerThisCur.bias()[j] * layerThatCur.bias()[j];
			if(dp < 0)
				invertNode(i, j); // invert it
		}
	}
}

void GNeuralNet::scaleWeights(double factor)
{
	GAssert(hasTrainingBegun());
	for(size_t i = m_layers.size() - 1; i < m_layers.size(); i--)
		scaleWeightsOneLayer(factor, i);
}

void GNeuralNet::scaleWeightsOneLayer(double factor, size_t lay)
{
	GNeuralNetLayer& l = *m_layers[lay];
	GMatrix& m = l.m_weights;
	size_t outputs = m.cols();
	for(size_t i = 0; i < m.rows(); i++)
		GVec::multiply(m[i], factor, outputs);
	GVec::multiply(l.bias(), factor, outputs);
}

void GNeuralNet::scaleWeightsSingleOutput(size_t output, double factor)
{
	GAssert(hasTrainingBegun());
	size_t lay = m_layers.size() - 1;
	GMatrix& m = m_layers[lay]->m_weights;
	GAssert(output < m.cols());
	for(size_t i = 0; i < m.rows(); i++)
		m[i][output] *= factor;
	m_layers[lay]->bias()[output] *= factor;
	for(lay--; lay < m_layers.size(); lay--)
	{
		GMatrix& m = m_layers[lay]->m_weights;
		size_t outputs = m.cols();
		for(size_t i = 0; i < m.rows(); i++)
			GVec::multiply(m[i], factor, outputs);
		GVec::multiply(m_layers[lay]->bias(), factor, outputs);
	}
}
#endif // MIN_PREDICT

void GNeuralNet::bleedWeights(double alpha)
{
	for(size_t i = m_layers.size() - 2; i < m_layers.size(); i--)
	{
		size_t layerSize = m_layers[i]->outputs();
		for(size_t j = 0; j < layerSize; j++)
		{
			// Compute sum-squared weights in next layer
			GNeuralNetLayer& layDownStream = *m_layers[i + 1];
			size_t dsOutputs = layDownStream.outputs();
			GMatrix& dsW = layDownStream.m_weights;
			double sswDownStream = GVec::squaredMagnitude(dsW[j], dsOutputs);

			// Compute sum-squared weights in this layer
			double sswUpStream = 0.0;
			GNeuralNetLayer& layUpStream = *m_layers[i];
			size_t usInputs = layUpStream.inputs();
			GMatrix& usW = layUpStream.m_weights;
			for(size_t k = 0; k < usInputs; k++)
				sswUpStream += (usW[k][j] * usW[k][j]);

			// Compute scaling factors
			double t1 = sqrt(sswDownStream);
			double t2 = sqrt(sswUpStream);
			double t3 = 4.0 * t1 * t2 * alpha;
			double t4 = sswUpStream + sswDownStream;
			double beta = (-t3 + sqrt(t3 * t3 - 4.0 * t4 * t4 * (alpha * alpha - 1.0))) / (2.0 * t4);
			double facDS = (beta * t1 + alpha * t2) / t1;
			double facUS = (beta * t2 + alpha * t1) / t2;

			// Scale the weights in both layers
			GVec::multiply(dsW[j], facDS, dsOutputs);
			for(size_t k = 0; k < usInputs; k++)
				usW[k][j] *= facUS;
		}
	}
}

void GNeuralNet::forwardProp(const double* pRow, size_t maxLayers)
{
	GNeuralNetLayer* pLay = m_layers[0];
	if(m_useInputBias)
		pLay->feedForwardWithInputBias(pRow);
	else
		pLay->feedForward(pRow);
	maxLayers = std::min(m_layers.size(), maxLayers);
	for(size_t i = 1; i < maxLayers; i++)
	{
		GNeuralNetLayer* pDS = m_layers[i];
		pDS->feedForward(pLay->activation());
		pLay = pDS;
	}
}

double GNeuralNet::forwardPropSingleOutput(const double* pRow, size_t output)
{
	if(m_layers.size() == 1)
	{
		GNeuralNetLayer& layer = *m_layers[0];
		layer.feedForwardToOneOutput(pRow, output, m_useInputBias);
		return layer.activation()[output];
	}
	else
	{
		GNeuralNetLayer* pLay = m_layers[0];
		if(m_useInputBias)
			pLay->feedForwardWithInputBias(pRow);
		else
			pLay->feedForward(pRow);
		for(size_t i = 1; i + 1 < m_layers.size(); i++)
		{
			GNeuralNetLayer* pDS = m_layers[i];
			pDS->feedForward(pLay->activation());
			pLay = pDS;
		}
		GNeuralNetLayer* pDS = m_layers[m_layers.size() - 1];
		pDS->feedForwardToOneOutput(pLay->activation(), output, false);
		return pDS->activation()[output];
	}
}

#ifndef MIN_PREDICT
// virtual
void GNeuralNet::predictDistributionInner(const double* pIn, GPrediction* pOut)
{
	throw Ex("Sorry, this model does not predict a distribution");
}
#endif // MIN_PREDICT

void GNeuralNet::copyPrediction(double* pOut)
{
	GNeuralNetLayer& outputLayer = *m_layers[m_layers.size() - 1];
	GVec::copy(pOut, outputLayer.activation(), outputLayer.outputs());
}

double GNeuralNet::sumSquaredPredictionError(const double* pTarget)
{
	GNeuralNetLayer& outputLayer = *m_layers[m_layers.size() - 1];
	return GVec::squaredDistance(pTarget, outputLayer.activation(), outputLayer.outputs());
}

// virtual
void GNeuralNet::predictInner(const double* pIn, double* pOut)
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	forwardProp(pIn);
	copyPrediction(pOut);
}

// virtual
void GNeuralNet::trainInner(const GMatrix& features, const GMatrix& labels)
{
	size_t validationRows = (size_t)(m_validationPortion * features.rows());
	size_t trainRows = features.rows() - validationRows;
	if(validationRows > 0)
	{
		GDataSplitter splitter(features, labels, m_rand, trainRows);
		trainWithValidation(splitter.features1(), splitter.labels1(), splitter.features2(), splitter.labels2());
	}
	else
		trainWithValidation(features, labels, features, labels);
}

#ifndef MIN_PREDICT
// virtual
void GNeuralNet::trainSparse(GSparseMatrix& features, GMatrix& labels)
{
	if(features.rows() != labels.rows())
		throw Ex("Expected the features and labels to have the same number of rows");
	GUniformRelation featureRel(features.cols());
	beginIncrementalLearning(featureRel, labels.relation());

	GTEMPBUF(size_t, indexes, features.rows());
	GIndexVec::makeIndexVec(indexes, features.rows());
	GTEMPBUF(double, pFullRow, features.cols());
	for(size_t epochs = 0; epochs < 100; epochs++) // todo: need a better stopping criterion
	{
		GIndexVec::shuffle(indexes, features.rows(), &m_rand);
		for(size_t i = 0; i < features.rows(); i++)
		{
			features.fullRow(pFullRow, indexes[i]);
			forwardProp(pFullRow);
			m_pBackProp->computeBlame(labels.row(indexes[i]), INVALID_INDEX, m_backPropTargetFunction);
			m_pBackProp->backpropagate();
			m_pBackProp->descendGradient(pFullRow, m_learningRate, m_momentum);
		}
	}
}
#endif // MIN_PREDICT

double GNeuralNet::validationSquaredError(const GMatrix& features, const GMatrix& labels)
{
	double sse = 0;
	size_t nCount = features.rows();
	for(size_t n = 0; n < nCount; n++)
	{
		forwardProp(features[n]);
		sse += sumSquaredPredictionError(labels[n]);
	}
	return sse;
}

size_t GNeuralNet::trainWithValidation(const GMatrix& trainFeatures, const GMatrix& trainLabels, const GMatrix& validateFeatures, const GMatrix& validateLabels)
{
	if(trainFeatures.rows() != trainLabels.rows() || validateFeatures.rows() != validateLabels.rows())
		throw Ex("Expected the features and labels to have the same number of rows");
	beginIncrementalLearningInner(trainFeatures.relation(), trainLabels.relation());

	// Do the epochs
	size_t nEpochs;
	double dBestError = 1e308;
	size_t nEpochsSinceValidationCheck = 0;
	double dSumSquaredError;
	GRandomIndexIterator ii(trainFeatures.rows(), m_rand);
	for(nEpochs = 0; true; nEpochs++)
	{
		ii.reset();
		size_t index;
		while(ii.next(index))
		{
			const double* pFeatures = trainFeatures[index];
			forwardProp(pFeatures);
			m_pBackProp->computeBlame(trainLabels[index], INVALID_INDEX, m_backPropTargetFunction);
			m_pBackProp->backpropagate();
			m_pBackProp->descendGradient(pFeatures, m_learningRate, m_momentum);
		}

		// Check for termination condition
		if(nEpochsSinceValidationCheck >= m_epochsPerValidationCheck)
		{
			nEpochsSinceValidationCheck = 0;
			dSumSquaredError = validationSquaredError(validateFeatures, validateLabels);
			if(1.0 - dSumSquaredError / dBestError < m_minImprovement)
				break;
			if(dSumSquaredError < dBestError)
				dBestError = dSumSquaredError;
		}
		else
			nEpochsSinceValidationCheck++;
	}

	releaseTrainingJunk();
	return nEpochs;
}

// virtual
void GNeuralNet::beginIncrementalLearningInner(const GRelation& featureRel, const GRelation& labelRel)
{
	if(labelRel.size() < 1)
		throw Ex("The label relation must have at least 1 attribute");

	// Make the layers
	clear();
	size_t inputs = featureRel.size() - (m_useInputBias ? 1 : 0);
	for(size_t i = 0; i < m_topology.size(); i++)
	{
		size_t outputs = m_topology[i];
		GNeuralNetLayer* pNewLayer = new GNeuralNetLayer(inputs, outputs);
		pNewLayer->resetWeights(&m_rand);
		m_layers.push_back(pNewLayer);
		inputs = outputs;
	}
	size_t outputs = labelRel.size();
	GNeuralNetLayer* pNewLayer = new GNeuralNetLayer(inputs, outputs);
	pNewLayer->resetWeights(&m_rand);
	m_layers.push_back(pNewLayer);

	// Make the training structures
	m_pBackProp = new GBackProp(this);
}

// virtual
void GNeuralNet::trainIncrementalInner(const double* pIn, const double* pOut)
{
	if(!hasTrainingBegun())
		throw Ex("train or beginIncrementalLearning must be called before this method");
	forwardProp(pIn);
	m_pBackProp->computeBlame(pOut, INVALID_INDEX, m_backPropTargetFunction);
	m_pBackProp->backpropagate();
	m_pBackProp->descendGradient(pIn, m_learningRate, m_momentum);
}


#ifndef MIN_PREDICT
void GNeuralNet::autoTune(GMatrix& features, GMatrix& labels)
{
	// Try a plain-old single-layer network
	size_t hidden = std::max((size_t)4, (features.cols() + 3) / 4);
	Holder<GNeuralNet> hCand0(new GNeuralNet());
	Holder<GNeuralNet> hCand1;
	double scores[2];
	scores[0] = hCand0.get()->crossValidate(features, labels, 2);
	scores[1] = 1e308;

	// Try increasing the number of hidden units until accuracy decreases twice
	size_t failures = 0;
	while(true)
	{
		GNeuralNet* cand = new GNeuralNet();
		vector<size_t> topology;
		topology.push_back(hidden);
		cand->setTopology(topology);
		double d = cand->crossValidate(features, labels, 2);
		if(d < scores[0])
		{
			hCand1.reset(hCand0.release());
			scores[1] = scores[0];
			hCand0.reset(cand);
			scores[0] = d;
		}
		else
		{
			if(d < scores[1])
			{
				hCand1.reset(cand);
				scores[1] = d;
			}
			else
				delete(cand);
			if(++failures >= 2)
				break;
		}
		hidden *= 4;
	}

	// Try narrowing in on the best number of hidden units
	while(true)
	{
		size_t a = hCand0.get()->layerCount() > 1 ? hCand0.get()->getLayer(0)->outputs() : 0;
		size_t b = hCand1.get()->layerCount() > 1 ? hCand1.get()->getLayer(0)->outputs() : 0;
		size_t dif = b < a ? a - b : b - a;
		if(dif <= 1)
			break;
		size_t c = (a + b) / 2;
		GNeuralNet* cand = new GNeuralNet();
		vector<size_t> topology;
		topology.push_back(c);
		cand->setTopology(topology);
		double d = cand->crossValidate(features, labels, 2);
		if(d < scores[0])
		{
			hCand1.reset(hCand0.release());
			scores[1] = scores[0];
			hCand0.reset(cand);
			scores[0] = d;
		}
		else if(d < scores[1])
		{
			hCand1.reset(cand);
			scores[1] = d;
		}
		else
		{
			delete(cand);
			break;
		}
	}
	hCand1.reset(NULL);

	// Try two hidden layers
	size_t hu1 = hCand0.get()->layerCount() > 1 ? hCand0.get()->getLayer(0)->outputs() : 0;
	size_t hu2 = 0;
	if(hu1 > 12)
	{
		size_t c1 = 16;
		size_t c2 = 16;
		if(labels.cols() < features.cols())
		{
			double d = sqrt(double(features.cols()) / labels.cols());
			c1 = std::max(size_t(9), size_t(double(features.cols()) / d));
			c2 = size_t(labels.cols() * d);
		}
		else
		{
			double d = sqrt(double(labels.cols()) / features.cols());
			c1 = size_t(features.cols() * d);
			c2 = std::max(size_t(9), size_t(double(labels.cols()) / d));
		}
		if(c1 < 16 && c2 < 16)
		{
			c1 = 16;
			c2 = 16;
		}
		GNeuralNet* cand = new GNeuralNet();
		vector<size_t> topology;
		topology.push_back(c1);
		topology.push_back(c2);
		cand->setTopology(topology);
		double d = cand->crossValidate(features, labels, 2);
		if(d < scores[0])
		{
			hCand0.reset(cand);
			scores[0] = d;
			hu1 = c1;
			hu2 = c2;
		}
		else
			delete(cand);
	}

	// Try with momentum
	{
		GNeuralNet* cand = new GNeuralNet();
		vector<size_t> topology;
		if(hu1 > 0) topology.push_back(hu1);
		if(hu2 > 0) topology.push_back(hu2);
		cand->setTopology(topology);
		cand->setMomentum(0.8);
		double d = cand->crossValidate(features, labels, 2);
		if(d < scores[0])
		{
			hCand0.reset(cand);
			scores[0] = d;
		}
		else
			delete(cand);
	}

	copyStructure(hCand0.get());
}
#endif // MIN_PREDICT

void GNeuralNet::normalizeInput(size_t index, double oldMin, double oldMax, double newMin, double newMax)
{
	if(m_useInputBias)
		throw Ex("normalizing input not supported with bias inputs");
	GNeuralNetLayer& layer = *m_layers[0];
	size_t outputs = layer.outputs();
	double* pW = layer.m_weights[index];
	double* pB = layer.bias();
	double f = (oldMax - oldMin) / (newMax - newMin);
	double g = (oldMin - newMin * f);
	for(size_t i = 0; i < outputs; i++)
	{
		*pB += (*pW * g);
		*pW *= f;
		pW++;
		pB++;
	}
}

void GNeuralNet::printWeights(std::ostream& stream)
{
	stream.precision(6);
	stream << "Neural Network:\n";
	for(size_t i = layerCount() - 1; i < layerCount(); i--)
	{
		if(i == layerCount() - 1)
			stream << "	Output Layer:\n";
		else
			stream << "	Hidden Layer " << to_str(i) << ":\n";
		GNeuralNetLayer& layer = *getLayer(i);
		for(size_t j = 0; j < layer.outputs(); j++)
		{
			stream << "		Unit " << to_str(j) << ":	";
			stream << "(bias: " << to_str(layer.bias()[j]) << ")	";
			for(size_t k = 0; k < layer.inputs(); k++)
			{
				if(k > 0)
					stream << "	";
				stream << to_str(layer.m_weights[k][j]);
			}
			stream << "\n";
		}
	}
}

GMatrix* GNeuralNet::compressFeatures(GMatrix& features)
{
	GNeuralNetLayer& lay = *getLayer(0);
	if(lay.inputs() != features.cols())
		throw Ex("mismatching number of data columns and layer units");
	GPCA pca(lay.inputs());
	pca.train(features);
	GMatrix* pBasis = pca.basis();//->transpose();
//	Holder<GMatrix> hBasis(pBasis);
	double* pOff = new double[lay.inputs()];
	ArrayHolder<double> hOff(pOff);
	pBasis->multiply(pca.centroid(), pOff);
	GMatrix* pInvTransform = pBasis->pseudoInverse();
	Holder<GMatrix> hInvTransform(pInvTransform);
	lay.transformWeights(*pInvTransform, pOff);
	return pca.transformBatch(features);
}


#ifndef MIN_PREDICT
void GNeuralNet_testMath()
{
	GMatrix features(0, 2);
	double* pVec = features.newRow();
	pVec[0] = 0.0;
	pVec[1] = -0.7;
	GMatrix labels(0, 1);
	labels.newRow()[0] = 1.0;

	// Make the Neural Network
	GNeuralNet nn;
	nn.setLearningRate(0.175);
	nn.setMomentum(0.9);
	vector<size_t> topology;
	topology.push_back(3);
	nn.setTopology(topology);
	nn.beginIncrementalLearning(features.relation(), labels.relation());
	if(nn.countWeights() != 13)
		throw Ex("Wrong number of weights");
	GNeuralNetLayer& layerOut = *nn.getLayer(1);
	layerOut.bias()[0] = 0.02; // w_0
	layerOut.m_weights[0][0] = -0.01; // w_1
	layerOut.m_weights[1][0] = 0.03; // w_2
	layerOut.m_weights[2][0] = 0.02; // w_3
	GNeuralNetLayer& layerHidden = *nn.getLayer(0);
	layerHidden.bias()[0] = -0.01; // w_4
	layerHidden.m_weights[0][0] = -0.03; // w_5
	layerHidden.m_weights[1][0] = 0.03; // w_6
	layerHidden.bias()[1] = 0.01; // w_7
	layerHidden.m_weights[0][1] = 0.04; // w_8
	layerHidden.m_weights[1][1] = -0.02; // w_9
	layerHidden.bias()[2] = -0.02; // w_10
	layerHidden.m_weights[0][2] = 0.03; // w_11
	layerHidden.m_weights[1][2] = 0.02; // w_12

	bool useCrossEntropy = false;

	// Test forward prop
	double tol = 1e-12;
	double pat[3];
	GVec::copy(pat, features[0], 2);
	nn.predict(pat, pat + 2);
	// Here is the math (done by hand) for why these results are expected:
	// Row: {0, -0.7, 1}
	// o_1 = squash(w_4*1+w_5*x+w_6*y) = 1/(1+exp(-(-.01*1-.03*0+.03*(-.7)))) = 0.4922506205862
	// o_2 = squash(w_7*1+w_8*x+w_9*y) = 1/(1+exp(-(.01*1+.04*0-.02*(-.7)))) = 0.50599971201659
	// o_3 = squash(w_10*1+w_11*x+w_12*y) = 1/(1+exp(-(-.02*1+.03*0+.02*(-.7)))) = 0.49150081873869
	// o_0 = squash(w_0*1+w_1*o_1+w_2*o_2+w_3*o_3) = 1/(1+exp(-(.02*1-.01*.4922506205862+.03*.50599971201659+.02*.49150081873869))) = 0.51002053349535
	//if(std::abs(pat[2] - 0.51002053349535) > tol) throw Ex("forward prop problem"); // logistic
	if(std::abs(pat[2] - 0.02034721575641) > tol) throw Ex("forward prop problem"); // tanh

	// Test that the output error is computed properly
	nn.trainIncremental(features[0], labels[0]);
	GBackProp* pBP = nn.backProp();
	// Here is the math (done by hand) for why these results are expected:
	// e_0 = output*(1-output)*(target-output) = .51002053349535*(1-.51002053349535)*(1-.51002053349535) = 0.1224456672531
	if(useCrossEntropy)
	{
		// Here is the math for why these results are expected:
		// e_0 = target-output = 1-.51002053349535 = 0.4899794665046473
		if(std::abs(pBP->layer(1).blame()[0] - 0.4899794665046473) > tol) throw Ex("problem computing output error");
	}
	else
	{
		// Here is the math for why these results are expected:
		// e_0 = output*(1-output)*(target-output) = .51002053349535*(1-.51002053349535)*(1-.51002053349535) = 0.1224456672531
		//if(std::abs(pBP->layer(1).blame()[0] - 0.1224456672531) > tol) throw Ex("problem computing output error"); // logistic
		if(std::abs(pBP->layer(1).blame()[0] - 0.9792471989888) > tol) throw Ex("problem computing output error"); // tanh
	}

	// Test Back Prop
	if(useCrossEntropy)
	{
		if(std::abs(pBP->layer(0).blame()[0] + 0.0012246544194742083) > tol) throw Ex("back prop problem");
		// e_2 = o_2*(1-o_2)*(w_2*e_0) = 0.00091821027577176
		if(std::abs(pBP->layer(0).blame()[1] - 0.0036743168717579557) > tol) throw Ex("back prop problem");
		// e_3 = o_3*(1-o_3)*(w_3*e_0) = 0.00061205143636003
		if(std::abs(pBP->layer(0).blame()[2] - 0.002449189448583718) > tol) throw Ex("back prop problem");
	}
	else
	{
		// e_1 = o_1*(1-o_1)*(w_1*e_0) = .4922506205862*(1-.4922506205862)*(-.01*.1224456672531) = -0.00030604063598154
		//if(std::abs(pBP->layer(0).blame()[0] + 0.00030604063598154) > tol) throw Ex("back prop problem"); // logistic
		if(std::abs(pBP->layer(0).blame()[0] + 0.00978306745006032) > tol) throw Ex("back prop problem"); // tanh
		// e_2 = o_2*(1-o_2)*(w_2*e_0) = 0.00091821027577176
		//if(std::abs(pBP->layer(0).blame()[1] - 0.00091821027577176) > tol) throw Ex("back prop problem"); // logistic
		if(std::abs(pBP->layer(0).blame()[1] - 0.02936050107376107) > tol) throw Ex("back prop problem"); // tanh
		// e_3 = o_3*(1-o_3)*(w_3*e_0) = 0.00061205143636003
		//if(std::abs(pBP->layer(0).blame()[2] - 0.00061205143636003) > tol) throw Ex("back prop problem"); // logistic
		if(std::abs(pBP->layer(0).blame()[2] - 0.01956232122115741) > tol) throw Ex("back prop problem"); // tanh
	}

	// Test weight update
	if(useCrossEntropy)
	{
		if(std::abs(layerOut.m_weights[0][0] - 0.10574640663831328) > tol) throw Ex("weight update problem");
		if(std::abs(layerOut.m_weights[1][0] - 0.032208721880745944) > tol) throw Ex("weight update problem");
	}
	else
	{
		// d_0 = (d_0*momentum)+(learning_rate*e_0*1) = 0*.9+.175*.1224456672531*1
		// w_0 = w_0 + d_0 = .02+.0214279917693 = 0.041427991769293
		//if(std::abs(layerOut.bias()[0] - 0.041427991769293) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerOut.bias()[0] - 0.191368259823049) > tol) throw Ex("weight update problem"); // tanh
		// d_1 = (d_1*momentum)+(learning_rate*e_0*o_1) = 0*.9+.175*.1224456672531*.4922506205862
		// w_1 = w_1 + d_1 = -.01+.0105479422563 = 0.00054794224635029
		//if(std::abs(layerOut.m_weights[0][0] - 0.00054794224635029) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerOut.m_weights[0][0] + 0.015310714964467731) > tol) throw Ex("weight update problem"); // tanh
		//if(std::abs(layerOut.m_weights[1][0] - 0.040842557664356) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerOut.m_weights[1][0] - 0.034112048752708297) > tol) throw Ex("weight update problem"); // tanh
		//if(std::abs(layerOut.m_weights[2][0] - 0.030531875498533) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerOut.m_weights[2][0] - 0.014175723281037968) > tol) throw Ex("weight update problem"); // tanh
		//if(std::abs(layerHidden.bias()[0] + 0.010053557111297) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerHidden.bias()[0] + 0.011712036803760557) > tol) throw Ex("weight update problem"); // tanh
		if(std::abs(layerHidden.m_weights[0][0] + 0.03) > tol) throw Ex("weight update problem"); // logistic & tanh
		//if(std::abs(layerHidden.m_weights[1][0] - 0.030037489977908) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerHidden.m_weights[1][0] - 0.03119842576263239) > tol) throw Ex("weight update problem"); // tanh
		//if(std::abs(layerHidden.bias()[1] - 0.01016068679826) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerHidden.bias()[1] - 0.015138087687908187) > tol) throw Ex("weight update problem"); // tanh
		if(std::abs(layerHidden.m_weights[0][1] - 0.04) > tol) throw Ex("weight update problem"); // logistic & tanh
		//if(std::abs(layerHidden.m_weights[1][1] + 0.020112480758782) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerHidden.m_weights[1][1] + 0.023596661381535732) > tol) throw Ex("weight update problem"); // tanh
		//if(std::abs(layerHidden.bias()[2] + 0.019892890998637) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerHidden.bias()[2] + 0.016576593786297455) > tol) throw Ex("weight update problem"); // tanh
		if(std::abs(layerHidden.m_weights[0][2] - 0.03) > tol) throw Ex("weight update problem"); // logistic & tanh
		//if(std::abs(layerHidden.m_weights[1][2] - 0.019925023699046) > tol) throw Ex("weight update problem"); // logistic
		if(std::abs(layerHidden.m_weights[1][2] - 0.01760361565040822) > tol) throw Ex("weight update problem"); // tanh
	}
}

void GNeuralNet_testInputGradient(GRand* pRand)
{
	for(int i = 0; i < 20; i++)
	{
		// Make the neural net
		GNeuralNet nn;
//		nn.addLayer(5);
//		nn.addLayer(10);
		GUniformRelation featureRel(5);
		GUniformRelation labelRel(10);
		nn.beginIncrementalLearning(featureRel, labelRel);

		// Init with random weights
		size_t weightCount = nn.countWeights();
		double* pWeights = new double[weightCount + 5 + 10 + 10 + 5 + 5];
		ArrayHolder<double> hWeights(pWeights);
		double* pFeatures = pWeights + weightCount;
		double* pTarget = pFeatures + 5;
		double* pOutput = pTarget + 10;
		double* pFeatureGradient = pOutput + 10;
		double* pEmpiricalGradient = pFeatureGradient + 5;
		for(size_t j = 0; j < weightCount; j++)
			pWeights[j] = pRand->normal() * 0.8;
		nn.setWeights(pWeights);

		// Compute target output
		GVec::setAll(pFeatures, 0.0, 5);
		nn.predict(pFeatures, pTarget);

		// Move away from the goal and compute baseline error
		for(int i = 0; i < 5; i++)
			pFeatures[i] += pRand->normal() * 0.1;
		nn.predict(pFeatures, pOutput);
		double sseBaseline = GVec::squaredDistance(pTarget, pOutput, 10);

		// Compute the feature gradient
		nn.forwardProp(pFeatures);
		nn.backProp()->computeBlame(pTarget);
		nn.backProp()->backpropagate();
		nn.backProp()->gradientOfInputs(pFeatureGradient);
		GVec::multiply(pFeatureGradient, 2.0, 5);

		// Empirically measure gradient
		for(int i = 0; i < 5; i++)
		{
			pFeatures[i] += 0.0001;
			nn.predict(pFeatures, pOutput);
			double sse = GVec::squaredDistance(pTarget, pOutput, 10);
			pEmpiricalGradient[i] = (sse - sseBaseline) / 0.0001;
			pFeatures[i] -= 0.0001;
		}

		// Check it
		double corr = GVec::correlation(pFeatureGradient, pEmpiricalGradient, 5);
		if(corr > 1.0)
			throw Ex("pathological results");
		if(corr < 0.999)
			throw Ex("failed");
	}
}

void GNeuralNet_testBinaryClassification(GRand* pRand)
{
	vector<size_t> vals;
	vals.push_back(2);
	GMatrix features(vals);
	GMatrix labels(vals);
	for(size_t i = 0; i < 100; i++)
	{
		double d = (double)pRand->next(2);
		features.newRow()[0] = d;
		labels.newRow()[0] = 1.0 - d;
	}
	GNeuralNet nn;
	nn.train(features, labels);
	double r = nn.sumSquaredError(features, labels);
	if(r > 0.0)
		throw Ex("Failed simple sanity test");
}

#define TEST_INVERT_INPUTS 5
void GNeuralNet_testInvertAndSwap(GRand& rand)
{
	size_t layers = 2;
	size_t layerSize = 5;

	// This test ensures that the GNeuralNet::swapNodes and GNeuralNet::invertNode methods
	// have no net effect on the output of the neural network
	double in[TEST_INVERT_INPUTS];
	double outBefore[TEST_INVERT_INPUTS];
	double outAfter[TEST_INVERT_INPUTS];
	for(size_t i = 0; i < 30; i++)
	{
		GNeuralNet nn;
		vector<size_t> topology;
		for(size_t j = 0; j < layers; j++)
			topology.push_back(layerSize);
		nn.setTopology(topology);
		GUniformRelation rel(TEST_INVERT_INPUTS);
		nn.beginIncrementalLearning(rel, rel);
		nn.perturbAllWeights(0.5);
		rand.cubical(in, TEST_INVERT_INPUTS);
		nn.predict(in, outBefore);
		for(size_t j = 0; j < 8; j++)
		{
			if(rand.next(2) == 0)
				nn.swapNodes((size_t)rand.next(layers), (size_t)rand.next(layerSize), (size_t)rand.next(layerSize));
			else
				nn.invertNode((size_t)rand.next(layers), (size_t)rand.next(layerSize));
		}
		nn.predict(in, outAfter);
		if(GVec::squaredDistance(outBefore, outAfter, TEST_INVERT_INPUTS) > 1e-10)
			throw Ex("Failed");
	}

	for(size_t i = 0; i < 30; i++)
	{
		// Generate two identical neural networks
		GNeuralNet nn1;
		GNeuralNet nn2;
		vector<size_t> topology;
		for(size_t j = 0; j < layers; j++)
			topology.push_back(layerSize);
		nn1.setTopology(topology);
		nn2.setTopology(topology);
		GUniformRelation rel(TEST_INVERT_INPUTS);
		nn1.beginIncrementalLearning(rel, rel);
		nn2.beginIncrementalLearning(rel, rel);
		nn1.perturbAllWeights(0.5);
		nn2.copyWeights(&nn1);

		// Predict something
		rand.cubical(in, TEST_INVERT_INPUTS);
		nn1.predict(in, outBefore);

		// Mess with the topology of both networks
		for(size_t j = 0; j < 20; j++)
		{
			if(rand.next(2) == 0)
			{
				if(rand.next(2) == 0)
					nn1.swapNodes((size_t)rand.next(layers), (size_t)rand.next(layerSize), (size_t)rand.next(layerSize));
				else
					nn1.invertNode((size_t)rand.next(layers), (size_t)rand.next(layerSize));
			}
			else
			{
				if(rand.next(2) == 0)
					nn2.swapNodes((size_t)rand.next(layers), (size_t)rand.next(layerSize), (size_t)rand.next(layerSize));
				else
					nn2.invertNode((size_t)rand.next(layers), (size_t)rand.next(layerSize));
			}
		}

		// Align the first network to match the second one
		nn1.align(nn2);

		// Check that predictions match before
		nn2.predict(in, outAfter);
		if(GVec::squaredDistance(outBefore, outAfter, TEST_INVERT_INPUTS) > 1e-10)
			throw Ex("Failed");
		nn1.predict(in, outAfter);
		if(GVec::squaredDistance(outBefore, outAfter, TEST_INVERT_INPUTS) > 1e-10)
			throw Ex("Failed");

		// Check that they have matching weights
		size_t wc = nn1.countWeights();
		double* pW1 = new double[wc];
		ArrayHolder<double> hW1(pW1);
		nn1.weights(pW1);
		double* pW2 = new double[wc];
		ArrayHolder<double> hW2(pW2);
		nn2.weights(pW2);
		for(size_t j = 0; j < wc; j++)
		{
			if(std::abs(*pW1 - *pW2) >= 1e-9)
				throw Ex("Failed");
			pW1++;
			pW2++;
		}
	}
}

void GNeuralNet_testNormalizeInput(GRand& rand)
{
	double in[5];
	for(size_t i = 0; i < 20; i++)
	{
		GNeuralNet nn;
		vector<size_t> topology;
		topology.push_back(5);
		nn.setTopology(topology);
		GUniformRelation relIn(5);
		GUniformRelation relOut(1);
		nn.beginIncrementalLearning(relIn, relOut);
		nn.perturbAllWeights(1.0);
		rand.spherical(in, 5);
		double before, after;
		nn.predict(in, &before);
		double a = rand.normal();
		double b = rand.normal();
		if(b < a)
			std::swap(a, b);
		double c = rand.normal();
		double d = rand.normal();
		if(d < c)
			std::swap(c, d);
		size_t ind = (size_t)rand.next(5);
		nn.normalizeInput(ind, a, b, c, d);
		in[ind] = GMatrix::normalizeValue(in[ind], a, b, c, d);
		nn.predict(in, &after);
		if(std::abs(after - before) > 1e-9)
			throw Ex("Failed");
	}
}

void GNeuralNet_testTrainOneLayer(GRand& rand)
{
	double in[5];
	rand.spherical(in, 5);
	double out[5];
	rand.spherical(out, 5);
	GNeuralNet nn1;
	vector<size_t> topology;
	topology.push_back(5);
	topology.push_back(5);
	topology.push_back(5);
	nn1.setTopology(topology);
	nn1.setUseInputBias(true);
	GUniformRelation rel(5, 0);
	nn1.beginIncrementalLearning(rel, rel);
	nn1.perturbAllWeights(3.0);
	GNeuralNet nn2;
	nn2.copyStructure(&nn1);
	nn2.copyWeights(&nn1);
	size_t wc = nn1.countWeights();
	double* pW1 = new double[2 * wc];
	ArrayHolder<double> hW1(pW1);
	double* pW2 = pW1 + wc;
	nn1.weights(pW1);
	nn2.weights(pW2);
	if(GVec::squaredDistance(pW1, pW2, wc) > 1e-12)
		throw Ex("copying error");
	nn1.forwardProp(in);
	nn1.backProp()->computeBlame(out);
	nn1.backProp()->backpropagate();
	nn1.backProp()->descendGradient(in, 0.2, 0.0);
	nn1.weights(pW1);
	if(GVec::squaredDistance(pW1, pW2, wc) < 1e-5)
		throw Ex("weight update problem");
	nn2.forwardProp(in);
	nn2.backProp()->computeBlame(out);
	nn2.backProp()->backpropagate();
	for(size_t l = 0; l < nn2.layerCount(); l++)
		nn2.backProp()->descendGradientOneLayer(l, in, 0.2, 0.0);
	nn2.weights(pW2);
	if(GVec::squaredDistance(pW1, pW2, wc) > 1e-12)
		throw Ex("problem with descendGradientOneLayer");
}

void GNeuralNet_testBleedWeights()
{
	GNeuralNet nn;
	vector<size_t> topology;
	topology.push_back(2);
	topology.push_back(2);
	nn.setTopology(topology);
	GUniformRelation rel(2, 0);
	nn.beginIncrementalLearning(rel, rel);
	nn.getLayer(2)->m_weights[0][0] = 1.0;
	nn.getLayer(2)->m_weights[1][0] = 1.0;
	nn.getLayer(2)->m_weights[0][1] = 1.0;
	nn.getLayer(2)->m_weights[1][1] = 1.0;
	nn.getLayer(1)->m_weights[0][0] = 5.0;
	nn.getLayer(1)->m_weights[1][0] = 2.0;
	nn.getLayer(1)->m_weights[0][1] = 3.0;
	nn.getLayer(1)->m_weights[1][1] = 1.0;
	nn.getLayer(0)->m_weights[0][0] = 0.5;
	nn.getLayer(0)->m_weights[1][0] = 0.2;
	nn.getLayer(0)->m_weights[0][1] = 0.3;
	nn.getLayer(0)->m_weights[1][1] = 0.1;
	size_t wc = nn.countWeights();
	double* pBefore = new double[wc];
	ArrayHolder<double> hBefore(pBefore);
	double* pAfter = new double[wc];
	ArrayHolder<double> hAfter(pAfter);
	nn.weights(pBefore);
	nn.bleedWeights(0.1);
	nn.weights(pAfter);
	if(std::abs(GVec::squaredMagnitude(pBefore, wc) - GVec::squaredMagnitude(pAfter, wc)) > 0.000001)
		throw Ex("failed");
}

void GNeuralNet_testTransformWeights(GRand& prng)
{
	for(size_t i = 0; i < 10; i++)
	{
		// Set up
		GNeuralNet nn;
		GUniformRelation in(2);
		GUniformRelation out(3);
		nn.beginIncrementalLearning(in, out);
		nn.perturbAllWeights(1.0);
		double x1[2];
		double x2[2];
		double y1[3];
		double y2[3];
		prng.spherical(x1, 2);

		// Predict normally
		nn.predict(x1, y1);

		// Transform the inputs and weights
		GMatrix transform(2, 2);
		prng.spherical(transform[0], 2);
		prng.spherical(transform[1], 2);
		double offset[2];
		prng.spherical(offset, 2);
		GVec::add(x1, offset, 2);
		transform.multiply(x1, x2, false);

		double tmp[2];
		GVec::multiply(offset, -1.0, 2);
		transform.multiply(offset, tmp);
		GVec::copy(offset, tmp, 2);
		GMatrix* pTransInv = transform.pseudoInverse();
		Holder<GMatrix> hTransInv(pTransInv);
		nn.getLayer(0)->transformWeights(*pTransInv, offset);

		// Predict again
		nn.predict(x2, y2);
		if(GVec::squaredDistance(y1, y2, 3) > 1e-15)
			throw Ex("transformWeights failed");
	}
}

void GNeuralNet_testCompressFeatures(GRand& prng)
{
	size_t dims = 5;
	GMatrix feat(50, dims);
	for(size_t i = 0; i < feat.rows(); i++)
		prng.spherical(feat[i], dims);

	// Set up
	GNeuralNet nn1;
	vector<size_t> topology;
	topology.push_back(dims * 2);
	nn1.setTopology(topology);
	nn1.beginIncrementalLearning(feat.relation(), feat.relation());
	nn1.perturbAllWeights(1.0);
	GNeuralNet nn2;
	nn2.copyStructure(&nn1);
	nn2.copyWeights(&nn1);

	// Test
	GMatrix* pNewFeat = nn1.compressFeatures(feat);
	Holder<GMatrix> hNewFeat(pNewFeat);
	double out1[dims];
	double out2[dims];
	for(size_t i = 0; i < feat.rows(); i++)
	{
		nn1.predict(pNewFeat->row(i), out1);
		nn2.predict(feat[i], out2);
		if(GVec::squaredDistance(out1, out2, dims) > 1e-14)
			throw Ex("failed");
	}
}

// static
void GNeuralNet::test()
{
	GRand prng(0);
	GNeuralNet_testMath();
	GNeuralNet_testBinaryClassification(&prng);
	GNeuralNet_testInputGradient(&prng);
	GNeuralNet_testInvertAndSwap(prng);
	GNeuralNet_testNormalizeInput(prng);
	GNeuralNet_testTrainOneLayer(prng);
	GNeuralNet_testBleedWeights();
	GNeuralNet_testTransformWeights(prng);
	GNeuralNet_testCompressFeatures(prng);

	// Test with no hidden layers (logistic regression)
	{
		GNeuralNet nn;
		nn.basicTest(0.74, 0.89);
	}

	// Test NN with one hidden layer
	{
		GNeuralNet nn;
		vector<size_t> topology;
		topology.push_back(3);
		nn.setTopology(topology);
		nn.basicTest(0.76, 0.9);
	}
}

#endif // MIN_PREDICT









GNeuralNetInverseLayer::~GNeuralNetInverseLayer()
{
	delete(m_pInverseWeights);
	delete(m_pActivationFunction);
}




GNeuralNetPseudoInverse::GNeuralNetPseudoInverse(GNeuralNet* pNN, double padding)
: m_padding(padding)
{
	size_t maxNodes = 0;
	size_t i;
	for(i = 0; i < pNN->layerCount(); i++)
	{
		GNeuralNetLayer& nnLayer = *pNN->getLayer(i);
		maxNodes = std::max(maxNodes, nnLayer.outputs());
		GNeuralNetInverseLayer* pLayer = new GNeuralNetInverseLayer();
		m_layers.push_back(pLayer);
		delete(pLayer->m_pActivationFunction);
		pLayer->m_pActivationFunction = nnLayer.activationFunction()->clone();
		GMatrix weights(nnLayer.outputs(), nnLayer.inputs());
		double* pBias = nnLayer.bias();
		GMatrix& weightsIn = nnLayer.m_weights;
		for(size_t j = 0; j < nnLayer.outputs(); j++)
		{
			double unbias = -*(pBias++);
			double* pRow = weights.row(j);
			for(size_t k = 0; k < nnLayer.inputs(); k++)
			{
				*(pRow++) = weightsIn[k][j];
				unbias -= nnLayer.activationFunction()->center() * weightsIn[k][j];
			}
			pLayer->m_unbias.push_back(unbias);
		}
		pLayer->m_pInverseWeights = weights.pseudoInverse();
	}
	m_pBuf1 = new double[2 * maxNodes];
	m_pBuf2 = m_pBuf1 + maxNodes;
}

GNeuralNetPseudoInverse::~GNeuralNetPseudoInverse()
{
	for(vector<GNeuralNetInverseLayer*>::iterator it = m_layers.begin(); it != m_layers.end(); it++)
		delete(*it);
	delete[] std::min(m_pBuf1, m_pBuf2);
}

void GNeuralNetPseudoInverse::computeFeatures(const double* pLabels, double* pFeatures)
{
	size_t inCount = 0;
	vector<GNeuralNetInverseLayer*>::iterator it = m_layers.end() - 1;
	GVec::copy(m_pBuf2, pLabels, (*it)->m_pInverseWeights->cols());
	for(; true; it--)
	{
		GNeuralNetInverseLayer* pLayer = *it;
		inCount = pLayer->m_pInverseWeights->rows();
		std::swap(m_pBuf1, m_pBuf2);

		// Invert the layer
		double* pT = m_pBuf1;
		for(vector<double>::iterator ub = pLayer->m_unbias.begin(); ub != pLayer->m_unbias.end(); ub++)
		{
			*pT = pLayer->m_pActivationFunction->inverse(*pT) + *ub;
			pT++;
		}
		pLayer->m_pInverseWeights->multiply(m_pBuf1, m_pBuf2);

		// Clip and uncenter the value
		pLayer = *it;
		double halfRange = pLayer->m_pActivationFunction->halfRange();
		double center = pLayer->m_pActivationFunction->center();
		pT = m_pBuf2;
		for(size_t i = 0; i < inCount; i++)
		{
			*pT = std::max(m_padding - halfRange, std::min(halfRange - m_padding, *pT)) + center;
			pT++;
		}

		if(it == m_layers.begin())
			break;
	}
	GVec::copy(pFeatures, m_pBuf2, inCount);
}

#ifndef MIN_PREDICT
// static
void GNeuralNetPseudoInverse::test()
{
	GNeuralNet nn;
	vector<size_t> topology;
	topology.push_back(5);
	topology.push_back(7);
	nn.setTopology(topology);
	GUniformRelation featureRel(3);
	GUniformRelation labelRel(12);
	nn.beginIncrementalLearning(featureRel, labelRel);
	for(size_t i = 0; i < nn.layerCount(); i++)
	{
		nn.getLayer(i)->setToWeaklyApproximateIdentity();
		nn.getLayer(i)->perturbWeights(nn.rand(), 0.5);
	}
	GNeuralNetPseudoInverse nni(&nn, 0.001);
	double labels[12];
	double features[3];
	double features2[3];
	for(size_t i = 0; i < 20; i++)
	{
		for(size_t j = 0; j < 3; j++)
			features[j] = nn.rand().uniform() * 0.98 + 0.01;
		nn.predict(features, labels);
		nni.computeFeatures(labels, features2);
		if(GVec::squaredDistance(features, features2, 3) > 1e-8)
			throw Ex("failed");
	}
}
#endif // MIN_PREDICT









GReservoirNet::GReservoirNet()
: GNeuralNet(), m_weightDeviation(0.5), m_augments(64), m_reservoirLayers(2)
{
	clearFeatureFilter();
}

GReservoirNet::GReservoirNet(GDomNode* pNode, GLearnerLoader& ll)
: GNeuralNet(pNode, ll)
{
	m_weightDeviation = pNode->field("wdev")->asDouble();
	m_augments = (size_t)pNode->field("augs")->asInt();
	m_reservoirLayers = (size_t)pNode->field("reslays")->asInt();
}

#ifndef MIN_PREDICT
// virtual
GDomNode* GReservoirNet::serialize(GDom* pDoc) const
{
	GDomNode* pNode = serializeInner(pDoc, "GReservoirNet");
	pNode->addField(pDoc, "wdev", pDoc->newDouble(m_weightDeviation));
	pNode->addField(pDoc, "augs", pDoc->newInt(m_augments));
	pNode->addField(pDoc, "reslays", pDoc->newInt(m_reservoirLayers));
	return pNode;
}

// virtual
void GReservoirNet::clearFeatureFilter()
{
	delete(m_pFilterFeatures);
	m_pFilterFeatures = new GDataAugmenter(new GReservoir(m_weightDeviation, m_augments, m_reservoirLayers));
}


// static
void GReservoirNet::test()
{
	GReservoirNet lr;
	lr.basicTest(0.73, 0.801);
}
#endif // MIN_PREDICT



} // namespace GClasses

