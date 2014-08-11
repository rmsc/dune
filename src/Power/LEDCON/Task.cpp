//***************************************************************************
// Copyright 2007-2014 Universidade do Porto - Faculdade de Engenharia      *
// Laboratório de Sistemas e Tecnologia Subaquática (LSTS)                  *
//***************************************************************************
// This file is part of DUNE: Unified Navigation Environment.               *
//                                                                          *
// Commercial Licence Usage                                                 *
// Licencees holding valid commercial DUNE licences may use this file in    *
// accordance with the commercial licence agreement provided with the       *
// Software or, alternatively, in accordance with the terms contained in a  *
// written agreement between you and Universidade do Porto. For licensing   *
// terms, conditions, and further information contact lsts@fe.up.pt.        *
//                                                                          *
// European Union Public Licence - EUPL v.1.1 Usage                         *
// Alternatively, this file may be used under the terms of the EUPL,        *
// Version 1.1 only (the "Licence"), appearing in the file LICENCE.md       *
// included in the packaging of this file. You may not use this work        *
// except in compliance with the Licence. Unless required by applicable     *
// law or agreed to in writing, software distributed under the Licence is   *
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF     *
// ANY KIND, either express or implied. See the Licence for the specific    *
// language governing permissions and limitations at                        *
// https://www.lsts.pt/dune/licence.                                        *
//***************************************************************************
// Author: Ricardo Martins                                                  *
//***************************************************************************

// ISO C++ 98 headers.
#include <string>
#include <cmath>

// DUNE headers.
#include <DUNE/DUNE.hpp>

namespace Power
{
  namespace LEDCON
  {
    using DUNE_NAMESPACES;

    //! %Task arguments.
    struct Arguments
    {
      //! Power channel.
      std::string pwr_chn;
      //! Slave system name.
      std::string slave_system;
      //! Slave entity name.
      std::string slave_entity;
    };

    struct Task: public Tasks::Task
    {
      //! Activation timer
      Counter<double> m_act_timer;
      //! True if slave CPU is alive.
      bool m_slave_alive;
      //! System id of the slave DUNE instance.
      unsigned m_slave_id;
      //! Task arguments.
      Arguments m_args;
      //! True if CCU induced power down is in progress.
      bool m_ccu_pdown;

      Task(const std::string& name, Tasks::Context& ctx):
        Tasks::Task(name, ctx),
        m_slave_alive(false),
        m_ccu_pdown(false)
      {
        // Define configuration parameters.
        paramActive(Tasks::Parameter::SCOPE_MANEUVER,
                    Tasks::Parameter::VISIBILITY_USER);

        param("Power Channel", m_args.pwr_chn)
        .description("Power channel");

        param("Slave System Name", m_args.slave_system)
        .description("Name of the slave system");

        param("Slave Entity Name", m_args.slave_entity)
        .description("Name of the slave entity");

        // Register handler routines.
        bind<IMC::PowerOperation>(this);
        bind<IMC::Heartbeat>(this);
      }

      void
      onUpdateParameters(void)
      {
        m_slave_id = resolveSystemName(m_args.slave_system);
      }

      void
      consume(const IMC::Heartbeat* msg)
      {
        if (!isActivating() || (msg->getSource() != m_slave_id))
          return;

        if (std::abs(msg->getTimeStamp() - Clock::getSinceEpoch()) <= 1.0)
        {
          debug("slave CPU is alive and synchronized");
          m_slave_alive = true;
        }
      }

      void
      consume(const IMC::PowerOperation* msg)
      {
        trace("power operation dest msg %d slave %d", msg->getDestination(), m_slave_id);
        if (msg->getDestination() == m_slave_id)
        {
          if (msg->op == IMC::PowerOperation::POP_PWR_UP)
          {
            trace("power operation up");
            m_ccu_pdown = false;
            sendPowerChannelControl(m_args.pwr_chn, true);
          }

          if (msg->op == IMC::PowerOperation::POP_PWR_DOWN)
          {
            trace("power operation down");
            sendPowerDown();
            m_ccu_pdown = true;
            m_act_timer.setTop(getDeactivationTime());
          }
        }
      }

      void
      sendPowerChannelControl(const std::string& name, bool value)
      {
        IMC::PowerChannelControl pcc;
        pcc.name = name;

        if (value)
          pcc.op = IMC::PowerChannelControl::PCC_OP_TURN_ON;
        else
          pcc.op = IMC::PowerChannelControl::PCC_OP_TURN_OFF;

        dispatch(pcc);
      }

      void
      setActiveParameter(bool value)
      {
        IMC::SetEntityParameters ep;
        ep.name = m_args.slave_entity;
        IMC::EntityParameter ea;
        ea.name = "Active";
        ea.value = value ? "true" : "false";
        ep.params.push_back(ea);
        dispatch(ep);
      }

      void
      onRequestActivation(void)
      {
        m_slave_alive = false;
        m_ccu_pdown = false;
        sendPowerChannelControl(m_args.pwr_chn, true);
        m_act_timer.setTop(getActivationTime());
      }

      void
      checkActivation(void)
      {
        if (!isActivating())
          return;

        if (m_act_timer.overflow())
        {
          activationFailed(DTR("failed to contact device"));
          sendPowerChannelControl(m_args.pwr_chn, false);
          return;
        }

        if (m_slave_alive)
        {
          activate();
          setActiveParameter(true);
          debug("activation took %0.2f s", getActivationTime() - m_act_timer.getRemaining());
        }
      }

      void
      onActivation(void)
      {
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);
      }

      void
      sendPowerDown(void)
      {
        IMC::PowerOperation pop;
        pop.setDestination(m_slave_id);
        pop.op = IMC::PowerOperation::POP_PWR_DOWN_IP;
        dispatch(pop);
      }

      void
      onRequestDeactivation(void)
      {
        setActiveParameter(false);
        sendPowerDown();
        m_act_timer.setTop(getDeactivationTime());
      }

      void
      checkDeactivation(void)
      {
        if (!isDeactivating())
          return;

        if (m_act_timer.overflow())
          deactivate();
      }

      void
      onDeactivation(void)
      {
        sendPowerChannelControl(m_args.pwr_chn, false);
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);
      }

      void
      checkPowerDown(void)
      {
        if (!m_ccu_pdown)
          return;

        if (m_act_timer.overflow())
        {
          sendPowerChannelControl(m_args.pwr_chn, false);
          setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);
        }
      }

      void
      onMain(void)
      {
        while (!stopping())
        {
          waitForMessages(1.0);

          if (isActive())
            setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);
          else
            setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);

          checkActivation();
          checkDeactivation();
          checkPowerDown();
        }
      }
    };
  }
}

DUNE_TASK
