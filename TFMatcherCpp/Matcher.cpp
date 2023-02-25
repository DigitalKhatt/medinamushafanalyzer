#include "Matcher.h"
namespace TFMatcherCpp {
	Matcher::Matcher(const Polygon& _polyA, const Polygon& _polyB)
	{
		turningFunctionA = TurningFunction(_polyA);

		auto tempB = TurningFunction(_polyB);

		AlignedTF matchedPair = AlignedTF(turningFunctionA, tempB);
		double distance = matchedPair.Distance();

		if (distance < 5) {
			reshapedTurningFunctionB = GenerateReshapedFunction(_polyB);
		}
		else {
			auto vertices = _polyB.GetVertices();
			std::reverse(std::begin(vertices), std::end(vertices));
			Polygon newB; //  {vertices};
			for (auto& vertex : vertices) {
				newB.AddVertex(vertex);
			}
			reshapedTurningFunctionB = GenerateReshapedFunction(newB);
		}

		

		
		//auto turningFunctionB = TurningFunction(_polyB);
		//reshapedTurningFunctionB.push_back(turningFunctionB);
	}

	Polygon Matcher::GenerateReshapedPolygon(const Polygon& _poly,
		int startIndex,
		int endIndex,
		int numVertices)
	{
		Polygon reshapedPoly = Polygon();

		for (int vertex = startIndex; vertex < numVertices; vertex++)
		{
			reshapedPoly.AddVertex(_poly.GetVertex(vertex));
		}

		for (int vertex = 0; vertex <= endIndex; vertex++)
		{
			reshapedPoly.AddVertex(_poly.GetVertex(vertex));
		}

		return reshapedPoly;
	}

	vector<TurningFunction> Matcher::GenerateReshapedFunction(const Polygon& _poly)
	{
		int numReshapedFuncs = _poly.GetEdgeCount();
		vector<TurningFunction> reshapedFunc(numReshapedFuncs);

		for (int poly = 0; poly < numReshapedFuncs; poly++)
		{
			int startIndex = poly;
			int endIndex = (poly + numReshapedFuncs) % numReshapedFuncs;

			Polygon reshapedPoly = GenerateReshapedPolygon(_poly, startIndex, endIndex, numReshapedFuncs);
			TurningFunction thisFunc = TurningFunction(reshapedPoly);

			reshapedFunc[poly] = thisFunc;
		}

		return reshapedFunc;
	}

	std::tuple<double,double,double> Matcher::Distance()
	{
		double minDistance = INT32_MAX;
		double complexity1 = turningFunctionA.GetComplexity();

		double complexity2 = 0;

		for (size_t i = 0; i < reshapedTurningFunctionB.size(); i++)
		{
			AlignedTF matchedPair = AlignedTF(turningFunctionA, reshapedTurningFunctionB[i]);
			double distance = matchedPair.Distance();

			if (distance < minDistance)
			{
				minDistance = distance;
				complexity2 = reshapedTurningFunctionB[i].GetComplexity();
			}
		}

		return { minDistance,complexity1,complexity2 };
	}
}