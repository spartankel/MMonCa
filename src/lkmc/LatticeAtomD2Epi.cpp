/*
 * LatticeAtomD2Epi.cpp
 *
 *  Created on: Jul 15, 2014
 *      Author: ignacio.martin@imdea.org
 *
 * Copyright 2014 IMDEA Materials Institute, Getafe, Madrid, Spain
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "LatticeAtomD2Epi.h"
#include "LatticeDiamondParam.h"
#include "LatticeDiamond.h"
#include "kernel/Domain.h"
#include "kernel/Mesh.h"
#include "kernel/MeshElement.h"
#include "kernel/RateManager.h"
#include "kernel/SubDomain.h"
#include "io/ParameterManager.h"

using std::vector;
using std::pair;
using std::map;

namespace LKMC {

double LatticeAtomD2Epi::_speed_howManyAdsorption = 0;
double LatticeAtomD2Epi::_speed_howManyDesorption = 0;
double LatticeAtomD2Epi::_speed_howManyPrecursor = 0;
float    LatticeAtomD2Epi::_speed_rateFactor = 1.;
float    LatticeAtomD2Epi::_speed_oldkt = 0;

LatticeAtomD2Epi::LatticeAtomD2Epi(LASTATE st, Kernel::Domain *p, Kernel::M_TYPE basicMt, Kernel::P_TYPE type, const Kernel::Coordinates &c, unsigned orientation)
 : LatticeAtomDiamond(st, p, basicMt, type, c, orientation)
{
	_atomPair = 0;
	_rateMigs = 0;
	insertNeighbors();  //this function cannot be in the base class because it needs the virtual classes working
}

LatticeAtomD2Epi::LatticeAtomD2Epi(std::istream &is) : LatticeAtomDiamond(is)
{
	_rateMigs = 0;
	insertNeighbors();
}


void LatticeAtomD2Epi::restart(std::ostream &os) const
{
	LatticeAtomDiamond::restart(os);
}

void LatticeAtomD2Epi::perform (Kernel::SubDomain *pSub, unsigned eventType)
{
	pSub->_evLog.performed(0, Event::LATTICEATOM, LSDT_DIAMOND_EPI1, 0, eventType, 0);
	_pDomain->_pRM->setDepthLA(_coord._x);
	map<int, LatticeAtomDiamond *> toUpdate;

	if(eventType < MAX_EPI_GASES)
		performPrecursor(pSub, _pDomain->_pEGPar[_basicMat]->_toPType[eventType], toUpdate);
	else if(eventType == MAX_EPI_GASES) //mig
		performMig(pSub, toUpdate);
	else if(eventType == MAX_EPI_GASES+1) //depositions or etching
		performEtching(pSub, toUpdate);
	else if(eventType == MAX_EPI_GASES+2)
		performAdsorption(pSub, toUpdate);
	else if(eventType == MAX_EPI_GASES+3)
		performDesorption(pSub, toUpdate);
	else
		ERRORMSG("LatticeAtomD2Epi::Unknown event " << eventType);

	for(map<int, LatticeAtomDiamond *>::iterator it=toUpdate.begin(); it!=toUpdate.end(); ++it)
		_pDomain->_pRM->update(it->second, static_cast<LatticeAtomD2Epi *>(it->second)->_pElement);
	if(_pElement->getNonCrystallineLA() == 0)
		_pDomain->_pLKMC->cleanLKMCAtoms(_pElement, MOD_Epitaxy);
}

//precursor desorption
void LatticeAtomD2Epi::performDesorption(Kernel::SubDomain *pSub, std::map<int, LatticeAtomDiamond *> &toUpdate)
{
	_speed_howManyDesorption++;
	_state = LS_AVAILABLE;
	toUpdate[_number] = this;
	if(_atomPair)
	{
		toUpdate[_atomPair->_number] = _atomPair;
		_atomPair->_atomPair = 0;
		_atomPair = 0;
	}
}

//takes an atom from substrate to "gas"
void LatticeAtomD2Epi::performEtching(Kernel::SubDomain *pSub, map<int, LatticeAtomDiamond *> &toUpdate)
{
	fillNeighborBoxes(pSub); //create atoms for this new configuration.
	_state = LS_AVAILABLE;
	_pElement->updateUnLAStatus(pSub, MOD_Epitaxy);
	toBeUpdated(this, toUpdate);
}

void LatticeAtomD2Epi::fillNeighborBoxes(Kernel::SubDomain *pSub)
{
	vector<Kernel::MeshElement *> neighbors;
	_pDomain->_pMesh->fillAdjacentNeighbors(_pElement, neighbors, _basicMat, Kernel::MAX_MATERIALS);
				for(vector<Kernel::MeshElement *>::iterator itN = neighbors.begin(); itN != neighbors.end(); ++itN)
					if(!(*itN)->getFirstLS())
						_pDomain->_pLKMC->putLKMCAtoms(pSub, *itN, _basicMat, MOD_Epitaxy);
}

void LatticeAtomD2Epi::fillWithAtoms(Kernel::SubDomain *pSub, LASTATE st, vector<pair<Kernel::Coordinates, unsigned> > &neis,
		map<int, LatticeAtomDiamond *> &toUpdate)
{
	vector<pair<Kernel::Coordinates, unsigned> > twins;
	const LatticeDiamond *pLat = static_cast<const LatticeDiamond *>(_pDomain->_pLat[_basicMat]);
	Kernel::Coordinates v[2];
	int nC=0;
	for(unsigned i=0; i<FIRSTN; ++i)
		if(_neighbors[i])
		{
			if(nC < 2)
			{
				v[nC] = _neighbors[i]->getCoordinates();
				_pDomain->_pMesh->setPeriodicRelative(_coord, v[nC]);
			}
			nC++;
		}
	if(nC == 2 || nC == 3) // 100
		_orientation = pLat->getNeis2(pSub, 0, v[0], v[1], neis);
	else if(nC == 1)//110 or 111 or something else
		 pLat->getNeis1(pSub, 0, v[0], _orientation, neis, twins); //returns an atom and their twins!
	else if(nC == 0)
		ERRORMSG("LatticeAtomD2Epi: Atom " << _number << " is not attached to anything!");
	createFromNeiList(pSub, st, neis, this, MOD_Epitaxy, toUpdate);
}

//alloy still not included!
//after the precursor is on the surface, then it attaches it to the substrate.
void LatticeAtomD2Epi::performAdsorption(Kernel::SubDomain *pSub, map<int, LatticeAtomDiamond *> &toUpdate)
{
	vector<pair<Kernel::Coordinates, unsigned> > neis;
	_speed_howManyAdsorption++;
	fillWithAtoms(pSub, LS_AVAILABLE, neis, toUpdate); //create atoms for this new configuration.
	_state = LS_PERFORMED;
	_pElement->updateLAStatus(pSub, MOD_Epitaxy);
	toBeUpdated(this, toUpdate);
	if(_atomPair)
	{
		LatticeAtomD2Epi *p = _atomPair;
		_atomPair->_atomPair = 0;
		_atomPair = 0;
		p->performAdsorption(pSub, toUpdate);
	}
}

void LatticeAtomD2Epi::performMig(Kernel::SubDomain *pSub, map<int, LatticeAtomDiamond *> &toUpdate)
{
	float totalMig = 0;
	for(unsigned idx=0; idx< _rateMigs; ++idx)
	{
		assert(_atomMig[idx]->getPerformed() == false);
		totalMig += _rateMig[idx];
	}
	float r = pSub->_rng.rand() * totalMig;
	//pick up the event
	unsigned event=0;
	for(event=0; event<_rateMigs; ++event)
		if(r < _rateMig[event])
			break;
		else
			r -= _rateMig[event];
	if(event == _rateMigs) //typical issue with floating point numbers,
		//pick up last.
	{
		event = 0;
		for(unsigned eve=0; eve<_rateMigs; ++eve)
			if (_rateMig[eve])
				event = eve;
	}
	assert(_rateMig[event]);
	_atomMig[event]->performPrecursor(pSub, _type, toUpdate);
	_atomMig[event]->performAdsorption(pSub, toUpdate);
	performEtching(pSub, toUpdate); //so far it ignores event
}

//It attaches a precursor to the interface
void LatticeAtomD2Epi::performPrecursor(Kernel::SubDomain *pSub, Kernel::P_TYPE pt, map<int, LatticeAtomDiamond *> &toUpdate)
{
	_speed_howManyPrecursor++;
	_type = pt;
	_state = LS_PRECURSOR;
	toUpdate[_number]=this;
	//it might form a "paired" precursor!
	const LatticeDiamondParam * param = static_cast<const LatticeDiamondParam *>(_pDomain->_pLaPar[_basicMat]);
	for(unsigned i=0; i<FIRSTN; ++i)
	{
		LatticeAtomD2Epi * laepi = static_cast<LatticeAtomD2Epi *>(_neighbors[i]);
		if( _neighbors[i] && laepi->_state == LS_PRECURSOR &&
		   laepi->_atomPair == 0  &&
		   laepi->_type == param->_epi[_type]._pairPrecursor)
			toUpdate[laepi->_number]=laepi;
	}
}

// eventType < MAX_EPI_GASES: Deposit the atom specified in the GasParameters;
// eventType == MAX_EPI_GASES: Migration
//           == MAX_EPI_GASES+1: etching
//           == MAX_EPI_GASES+2: final adsorption
//           == MAX_EPI_GASES+3: precursor desorption
float LatticeAtomD2Epi::getRate(unsigned eventType, float kT) const
{
	if((eventType <    MAX_EPI_GASES && _state != LS_AVAILABLE) || //precursor depo
	   (eventType ==   MAX_EPI_GASES && _state != LS_PERFORMED) || //mig
	   (eventType == 1+MAX_EPI_GASES && _state != LS_PERFORMED) || //final product etching
	   (eventType == 2+MAX_EPI_GASES && _state != LS_PRECURSOR) || //final absorption
	   (eventType == 3+MAX_EPI_GASES && _state != LS_PRECURSOR))   //precursor desorption
		return 0;

	const LatticeDiamondParam * param = static_cast<const LatticeDiamondParam *>(_pDomain->_pLaPar[_basicMat]);

	Kernel::ID neighborhood;
	neighborhood._mt = _basicMat;
	unsigned neigStates[3] = { 0, 0, 0 }; //array with neighboring states
	for(unsigned i=0; i<FIRSTN; ++i)
		if(_neighbors[i])
			neigStates[static_cast<LatticeAtomD2Epi *>(_neighbors[i])->_state]++;
	if( (eventType <    MAX_EPI_GASES && neigStates[LS_PERFORMED] == 0) ||
	    (eventType == 1+MAX_EPI_GASES && neigStates[LS_AVAILABLE] == 0))
		return 0;
	assert(eventType != 2+MAX_EPI_GASES || neigStates[LS_PERFORMED]);
	Kernel::P_TYPE myType = (eventType < MAX_EPI_GASES ? _pDomain->_pEGPar[_basicMat]->_toPType[eventType]: _type);
	if(myType == Kernel::UNDEFINED_TYPE) //gas not defined.
		return 0;

	const float maxRatio = param->_epi[myType]._speedUpRatio;
	if(_speed_oldkt != kT)
	{
		_speed_oldkt = kT;
		_speed_rateFactor = 1;
	}
	if(maxRatio && _speed_howManyPrecursor  + _speed_howManyAdsorption + _speed_howManyDesorption > 50000)
	{
		//LOWMSG2("speedRateFactor es " << _speed_rateFactor);
		if(_speed_howManyAdsorption == 0) //to avoid FPE.
			_speed_howManyAdsorption = 1e-10;
		if (_speed_howManyPrecursor / _speed_howManyAdsorption > maxRatio &&
		    _speed_howManyDesorption / _speed_howManyAdsorption > maxRatio)
			_speed_rateFactor /= 2;
		else if(_speed_howManyPrecursor / _speed_howManyAdsorption < maxRatio ||
				_speed_howManyDesorption / _speed_howManyAdsorption < maxRatio)
		{
			if(_speed_rateFactor < 0.5 )
				_speed_rateFactor*=2;
			else
				_speed_rateFactor = 1.;
		}
		_speed_howManyAdsorption = _speed_howManyDesorption = _speed_howManyPrecursor = 0;
	}

	if(eventType < MAX_EPI_GASES) //precursor attachment
	{
		float pref = _speed_rateFactor * _pDomain->_pEGPar[_basicMat]->_prefPrecursor[myType]; //yes, computed by this one to include pressure and temperature
		float ener = param->_epi[myType]._barrierPrecursor;
		return pref*exp(-ener/kT);
	}
	if(eventType == 3+MAX_EPI_GASES) //precursor desorption
	{
		float pref = _speed_rateFactor * param->_epi[myType]._prefDes;
		float ener = param->_epi[myType]._barrierDes;
		return pref*exp(-ener/kT);
	}
	float binding = 0;
	for(unsigned i=0; i<THIRDN; ++i)
		if(_neighbors[i] && _neighbors[i]->getPerformed())
		{
			Kernel::P_TYPE pt = _neighbors[i]->getPType();
			if(i < FIRSTN)
			{
				std::map<Kernel::P_TYPE, unsigned>::iterator it = neighborhood._pt.find(pt);
				if(it == neighborhood._pt.end())
					neighborhood._pt[pt] = 1;
				else
					it->second++;
			}
			else if(i<SECONDN)
				binding += param->_epi[myType]._formationSecond[pt];
			else
				binding += param->_epi[myType]._formationThird[pt];
		}

	if(neighborhood._pt.size() != 0) //requires at least one atom to attach to.
	{
		unsigned hash = param->_epi[myType]._formationFirst.cluster2hash(neighborhood);
		if(hash == param->_epi[myType]._formationFirst.invalidHash())
			WARNINGMSG("Diamond Epitaxy: Parameters not specified for " <<
				Domains::global()->PM()->getParticleName(neighborhood._mt, myType) << "+" <<
				Domains::global()->PM()->getEpiIDName(neighborhood) << ". Taking 0.");
		else
			binding += param->_epi[myType]._formationFirst._map[hash];
	}

	float pref = 0;
	float ener = 0;

	if(eventType == MAX_EPI_GASES) //migration
		return param->_model_simplified? 0 : getMigRate(binding, kT);
	else if(eventType == MAX_EPI_GASES+1) //final product etching
	{
		pref = param->_epi[myType]._prefEtch;
		if(!param->_model_simplified)
			ener = -binding;
	}
	else if(eventType == MAX_EPI_GASES+2) //attachment of a precursor.
	{
		if(neighborhood._pt.size() == 0)
			return 0;
		pref = param->_epi[myType]._prefEpi;
		float barrier = param->_epi[myType]._barrierEpi;
		//is there a second precursor for a pair?
		if(neigStates[LS_PRECURSOR])
			for(unsigned i=0; i<FIRSTN; ++i)
			{
				LatticeAtomD2Epi * laepi = static_cast<LatticeAtomD2Epi *>(_neighbors[i]);
					if( _neighbors[i] && laepi->_state == LS_PRECURSOR &&
					   (laepi->_atomPair == 0 || laepi->_atomPair == this) &&
						laepi->_type == param->_epi[myType]._pairPrecursor)
					{
						barrier = param->_epi[myType]._barrierPairEpi;
						_atomPair = laepi;
						_atomPair->_atomPair = const_cast<LatticeAtomD2Epi *>(this);
						break;
					}
			}
		ener = barrier + (param->_model_simplified? binding:0);
	}
	else
		ERRORMSG("Event type not recognized in LatticeAtomD2Epi");
	return pref*exp(-ener/kT);
}

//tempEner is the energy the atom has in the current position.
double LatticeAtomD2Epi::getMigRate(float tempEner, float kT) const
{
	const LatticeDiamondParam * param = static_cast<const LatticeDiamondParam *>(_pDomain->_pLaPar[_basicMat]);
	// Look for candidates. First neighbors and second neighbors.
	float returnRate = 0;
	_rateMigs = 0;
	for(unsigned i=0; i<SECONDN; ++i)
		if(_neighbors[i] && static_cast<LatticeAtomD2Epi *>(_neighbors[i])->_state == LS_AVAILABLE)
		{
			_atomMig[_rateMigs] = static_cast<LatticeAtomD2Epi *>(_neighbors[i]); //candidate for "moving"
			LatticeAtomD2Epi *candidate = _atomMig[_rateMigs];
			//needs to have a first neighbor that is not me!
			unsigned j=0;
			for(; j<FIRSTN; ++j)
				//if belongs to the interface
				if(candidate->_neighbors[j] && candidate->_neighbors[j] != this && candidate->_neighbors[j]->getPerformed())
					break;
			if(j == FIRSTN) //didn't work, does not belong to an interface
				continue;
			//let's compute the final energy
			Kernel::ID neighborhood;
			neighborhood._mt = _basicMat;
			float endEner = 0;
			for(j=0; j<THIRDN; ++j)
			{
				LatticeAtom * pLA = candidate->_neighbors[j];
				if(pLA  && pLA != this && pLA->getPerformed()) //should not double account -> pLA != this
				{
					Kernel::P_TYPE pt = pLA->getPType();
					if(j < FIRSTN)
					{
						std::map<Kernel::P_TYPE, unsigned>::iterator it = neighborhood._pt.find(pt);
						if(it == neighborhood._pt.end())
							neighborhood._pt[pt] = 1;
						else
							it->second++;
					}
					else if(j<SECONDN)
						endEner += param->_epi[_type]._formationSecond[pt];
					else
						endEner += param->_epi[_type]._formationThird[pt];
				}
			}
			if(neighborhood._pt.size() != 0)
			{
				assert(neighborhood._pt.size() == 1);
				unsigned hash = param->_epi[_type]._formationFirst.cluster2hash(neighborhood);
				if(hash != param->_epi[_type]._formationFirst.invalidHash())
					endEner += param->_epi[_type]._formationFirst._map[hash];
			}

			//OK, migration energy
			float barrier = i < FIRSTN? param->_epi[_type]._migrationF : param->_epi[_type]._migrationS;
			float ener = std::max(endEner - tempEner, float(0.)) + barrier;
			_rateMig[_rateMigs] = param->_epi[_type]._prefMig * exp(-ener/kT);
			returnRate += _rateMig[_rateMigs];
			if(++_rateMigs == MAX_ATOM_MIGS)
				break;
		}
	return returnRate;
}

} /* namespace LKMC */
