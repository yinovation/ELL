////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Machine Learning Library (EMLL)
//  File:     ForestNode.cpp (nodes)
//  Authors:  Ofer Dekel
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "ForestNode.h"
#include "ConstantNode.h"
#include "ElementSelectorNode.h"
#include "BinaryOperationNode.h"
#include "SingleElementThresholdNode.h"
#include "SumNode.h"

// stl
#include <vector>
#include <memory>

namespace nodes
{
    template<typename SplitRuleType, typename EdgePredictorType>
    ForestNode<SplitRuleType, EdgePredictorType>::ForestNode(const model::PortElements<double>& input, const predictors::ForestPredictor<SplitRuleType, EdgePredictorType>& forest) : Node({ &_input }, { &_output, &_treeOutputs, &_edgeIndicatorVector }), _input(this, input, inputPortName), _output(this, outputPortName, 1), _treeOutputs(this, treeOutputsPortName, forest.NumTrees()), _edgeIndicatorVector(this, edgeIndicatorVectorPortName, forest.NumEdges()), _forest(forest)
    {}

    template<typename SplitRuleType, typename EdgePredictorType>
    void ForestNode<SplitRuleType, EdgePredictorType>::Copy(model::ModelTransformer& transformer) const
    {
        auto newPortElements = transformer.TransformPortElements(_input.GetPortElements());
        auto newNode = transformer.AddNode<ForestNode<SplitRuleType, EdgePredictorType>>(newPortElements, _forest);
        transformer.MapOutputPort(output, newNode->output);
        transformer.MapOutputPort(treeOutputs, newNode->treeOutputs);
        transformer.MapOutputPort(edgeIndicatorVector, newNode->edgeIndicatorVector);
    }

    template<typename SplitRuleType, typename EdgePredictorType>
    void ForestNode<SplitRuleType, EdgePredictorType>::Refine(model::ModelTransformer & transformer) const
    {
        auto newPortElements = transformer.TransformPortElements(_input.GetPortElements());      
        const auto& interiorNodes = _forest.GetInteriorNodes();

        // create a place to store references to the output ports of the sub-models at each interior node
        // TODO: waiting for PortElements changes: this should be a vector of PortElements and not PortRange -it is currently a vector of Ranges because I need to AddRange them
        std::vector<model::PortRange> interiorNodeSubModels(interiorNodes.size());

        // visit interior nodes bottom-up (in reverse topological order)
        for(size_t i = interiorNodes.size(); i > 0; --i)
        {
            const auto& edges = interiorNodes[i - 1].GetOutgoingEdges();

            // get the sub-model that represents each outgoing edge
            // TODO: waiting for PortElements changes: why is this an PortElements, while interiorNodeSubModels is a vector?
            model::PortElements<double> edgeOutputs;
            for(size_t j = 0; j < edges.size(); ++j)
            {
                const auto& edgePredictor = edges[j].GetPredictor();
                auto edgePredictorNode = AddNodeToModelTransformer(newPortElements, edgePredictor, transformer);

                if(edges[j].IsTargetInterior()) // target node is itself an interior node: reverse topological order guarantees that it's already visited
                {
                    // TODO: waiting for PortElements changes: the following 3 lines should be one clean line - there are currently 3 because interiorNodeSubModels had to be an array of Ranges
                    model::PortElements<double> elements;
                    auto targetNodeSubModelOutputs = interiorNodeSubModels[edges[j].GetTargetNodeIndex()];
                    elements.AddRange(targetNodeSubModelOutputs);

                    auto sumNode = transformer.AddNode<BinaryOperationNode<double>>(edgePredictorNode->output, elements, BinaryOperationNode<double>::OperationType::add);
                    edgeOutputs.AddRange(sumNode->output);
                }
                else // target node is a leaf
                {
                    edgeOutputs.AddRange(edgePredictorNode->output);
                }
            }

            // add the sub-model that computes the split rule
            auto splitRuleNode = AddNodeToModelTransformer(newPortElements, interiorNodes[i - 1].GetSplitRule(), transformer);

            auto selectorNode = transformer.AddNode<ElementSelectorNode<double, bool>>(edgeOutputs, splitRuleNode->output);
            interiorNodeSubModels[i-1] = selectorNode->output;
        }

        // collect the sub-models that represent the trees of the forest
        // TODO: waiting for PortElements changes: why is this an PortElements, while interiorNodeSubModels is a vector?
        model::PortElements<double> treeSubModels;
        for(size_t rootIndex : _forest.GetRootIndices())
        {
            treeSubModels.AddRange(interiorNodeSubModels[rootIndex]);
        }

        // add the bias term
        auto biasNode = transformer.AddNode<ConstantNode<double>>(_forest.GetBias());
        treeSubModels.AddRange(biasNode->output);

        // sum all of the trees
        auto sumNode = transformer.AddNode<SumNode<double>>(treeSubModels);
        
        transformer.MapOutputPort(output, sumNode->output);
        // TODO: waiting for PortElements changes:
        //       transformer.MapOutputPort(treeOutputs, ...);
        //       transformer.MapOutputPort(edgeIndicatorVector, ...);
    }

    template<typename SplitRuleType, typename EdgePredictorType>
    void ForestNode<SplitRuleType, EdgePredictorType>::Compute() const
    {
        // forest output
        _output.SetOutput({ _forest.Predict(_input) });

        // individual tree outputs
        std::vector<double> treeOutputs(_forest.NumTrees());
        for(size_t i=0; i<_forest.NumTrees(); ++i)
        {
            treeOutputs[i] = _forest.Predict(_input, _forest.GetRootIndex(i));
        }
        _treeOutputs.SetOutput(std::move(treeOutputs));

        // path indicator
        auto edgeIndicator = _forest.GetEdgeIndicatorVector(_input);
        _edgeIndicatorVector.SetOutput(std::move(edgeIndicator));
    }
}